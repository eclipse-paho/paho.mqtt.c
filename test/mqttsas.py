"""
*******************************************************************
  Copyright (c) 2013, 2020 IBM Corp.

  All rights reserved. This program and the accompanying materials
  are made available under the terms of the Eclipse Public License v2.0
  and Eclipse Distribution License v1.0 which accompany this distribution.

  The Eclipse Public License is available at
     https://www.eclipse.org/legal/epl-2.0/
  and the Eclipse Distribution License is available at
    http://www.eclipse.org/org/documents/edl-v10.php.

  Contributors:
     Ian Craggs - initial implementation and/or documentation
     Ian Craggs - add MQTTV5 support
*******************************************************************
"""
from __future__ import print_function

import socket
import sys
import select
import traceback
import datetime
import os
import base64
import hashlib
import logging
try:
    import socketserver
    import MQTTV311    # Trace MQTT traffic - Python 3 version
    import MQTTV5
except:
    traceback.print_exc()
    import SocketServer as socketserver
    import MQTTV3112 as MQTTV311   # Trace MQTT traffic - Python 2 version
    import MQTTV5

MQTT = MQTTV311
logging = True
myWindow = None


class BufferedSockets:

    def __init__(self, socket):
        self.socket = socket
        self.buffer = bytearray()
        self.websockets = False
        
    def close(self):
        self.socket.shutdown(socket.SHUT_RDWR)
        self.socket.close()

    def rebuffer(self, data):
        self.buffer = data + self.buffer

    def wsrecv(self):
        try:
            header1 = ord(self.socket.recv(1))
            header2 = ord(self.socket.recv(1))
        except:
            return

        opcode = (header1 & 0x0f)
        maskbit = (header2 & 0x80) == 0x80
        length = (header2 & 0x7f)  # works for 0 to 125 inclusive
        if length == 126:  # for 126 to 65535 inclusive
            lb1 = ord(self.socket.recv(1))
            lb2 = ord(self.socket.recv(1))
            length = lb1*256 + lb2
        elif length == 127:
            length = 0
            for i in range(0, 8):
                length += ord(self.socket.recv(1)) * 2**((7 - i)*8)
        assert maskbit == True
        if maskbit:
            mask = self.socket.recv(4)
        mpayload = bytearray()
        while len(mpayload) < length:
            mpayload += self.socket.recv(length - len(mpayload))
        buffer = bytearray()
        if maskbit:
            mi = 0
            for i in mpayload:
                buffer.append(i ^ mask[mi])
                mi = (mi+1) % 4
        else:
            buffer = mpayload
        self.buffer += buffer

    def recv(self, bufsize):
        if self.websockets:
            while len(self.buffer) < bufsize:
                self.wsrecv()
            out = self.buffer[:bufsize]
            self.buffer = self.buffer[bufsize:]
        else:
            if bufsize <= len(self.buffer):
                out = self.buffer[:bufsize]
                self.buffer = self.buffer[bufsize:]
            else:
                out = self.buffer + \
                    self.socket.recv(bufsize - len(self.buffer))
                self.buffer = bytes()
        return out

    def __getattr__(self, name):
        return getattr(self.socket, name)

    def send(self, data):
        header = bytearray()
        if self.websockets:
            header.append(0x82)  # opcode
            l = len(data)
            if l < 126:
                header.append(l)
            elif l < 65536:
                """ If 126, the following 2 bytes interpreted as a 16-bit unsigned integer are
                    the payload length.
                """
                header += bytearray([126, l // 256, l % 256])
            elif l < 2**64:
                """ If 127, the following 8 bytes interpreted as a 64-bit unsigned integer (the
                    most significant bit MUST be 0) are the payload length.
                """
                mybytes = [127]
                for i in range(0, 7):
                    divisor = 2**((7 - i)*8)
                    mybytes.append(l // divisor)
                    l %= divisor
                mybytes.append(l)  # units
                header += bytearray(mybytes)
        totaldata = header + data
        # Ensure the entire packet is sent by calling send again if necessary
        sent = self.socket.send(totaldata)
        while sent < len(totaldata):
            sent += self.socket.send(totaldata[sent:])
        return sent


def timestamp():
    now = datetime.datetime.now()
    return now.strftime('%Y%m%d %H%M%S')+str(float("."+str(now.microsecond)))[1:]


suspended = []


class MyHandler(socketserver.StreamRequestHandler):

    def getheaders(self, data):
        "return headers: keys are converted to upper case so that checks are case insensitive"
        headers = {}
        lines = data.splitlines()
        for curline in lines[1:]:
            if curline.find(":") != -1:
                key, value = curline.split(": ", 1)
                headers[key.upper()] = value     # headers are case insensitive
        return headers

    def handshake(self, client):
        GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
        data = client.recv(1024).decode('utf-8')
        headers = self.getheaders(data)
        digest = base64.b64encode(hashlib.sha1(
            (headers['SEC-WEBSOCKET-KEY'] + GUID).encode("utf-8")).digest())
        resp = b"HTTP/1.1 101 Switching Protocols\r\n" +\
               b"Upgrade: websocket\r\n" +\
               b"Connection: Upgrade\r\n" +\
               b"Sec-WebSocket-Protocol: mqtt\r\n" +\
               b"Sec-WebSocket-Accept: " + digest + b"\r\n\r\n"
        return client.send(resp)

    def handle(self):
        global MQTT
        if not hasattr(self, "ids"):
            self.ids = {}
        if not hasattr(self, "versions"):
            self.versions = {}
        inbuf = True
        first = True
        i = o = e = None
        try:
            clients = BufferedSockets(self.request)
            sock_no = clients.fileno()
            brokers = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            brokers.connect((brokerhost, brokerport))
            terminated = False
            while inbuf != None and not terminated:
                (i, o, e) = select.select([clients, brokers], [], [])
                for s in i:
                    if s in suspended:
                        print("suspended")
                    if s == clients and s not in suspended:
                        if first:
                            char = clients.recv(1)
                            clients.rebuffer(char)
                            if char == b"G":    # should be websocket connection
                                self.handshake(clients)
                                clients.websockets = True
                                print("Switching to websockets for socket %d" % sock_no)
                        inbuf = MQTT.getPacket(clients)  # get one packet
                        if inbuf == None:
                            break
                        try:
                            # if connect, this could be MQTTV3 or MQTTV5
                            if inbuf[0] >> 4 == 1:  # connect packet
                                protocol_string = b'MQTT'
                                pos = inbuf.find(protocol_string)
                                if pos != -1:
                                    version = inbuf[pos +
                                                    len(protocol_string)]
                                    if version == 5:
                                        MQTT = MQTTV5
                                    else:
                                        MQTT = MQTTV311
                            packet = MQTT.unpackPacket(inbuf)
                            if hasattr(packet.fh, "MessageType"):
                                packet_type = packet.fh.MessageType
                                publish_type = MQTT.PUBLISH
                                connect_type = MQTT.CONNECT
                            else:
                                packet_type = packet.fh.PacketType
                                publish_type = MQTT.PacketTypes.PUBLISH
                                connect_type = MQTT.PacketTypes.CONNECT
                            if packet_type == publish_type and \
                                    packet.topicName == "MQTTSAS topic" and \
                                    packet.data == b"TERMINATE":
                                print("Terminating client", self.ids[id(clients)])
                                brokers.close()
                                clients.close()
                                terminated = True
                                break
                            elif packet_type == publish_type and \
                                    packet.topicName == "MQTTSAS topic" and \
                                    packet.data == b"TERMINATE_SERVER":
                                print("Suspending client ", self.ids[id(clients)])
                                suspended.append(clients)
                            elif packet_type == connect_type:
                                self.ids[id(clients)
                                         ] = packet.ClientIdentifier
                                self.versions[id(clients)] = 3
                            print(timestamp(), "C to S",
                                  self.ids[id(clients)], str(packet))
                            #print([hex(b) for b in inbuf])
                            # print(inbuf)
                        except:
                            traceback.print_exc()
                        brokers.send(inbuf)       # pass it on
                    elif s == brokers:
                        inbuf = MQTT.getPacket(brokers)  # get one packet
                        if inbuf == None:
                            break
                        try:
                            print(timestamp(), "S to C", self.ids[id(clients)], str(MQTT.unpackPacket(inbuf)))
                        except:
                            traceback.print_exc()
                        clients.send(inbuf)
            print(timestamp()+" client " + self.ids[id(clients)]+" connection closing")
            first = False
        except:
            print(repr((i, o, e)), repr(inbuf))
            traceback.print_exc()
        if id(clients) in self.ids.keys():
            del self.ids[id(clients)]
        elif id(clients) in self.versions.keys():
            del self.versions[id(clients)]


class ThreadingTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    pass


def run():
    global brokerhost, brokerport
    myhost = '127.0.0.1'
    if len(sys.argv) > 1:
        brokerhost = sys.argv[1]
    else:
        brokerhost = '127.0.0.1'

    if len(sys.argv) > 2:
        brokerport = int(sys.argv[2])
    else:
        brokerport = 1883

    if len(sys.argv) > 3:
        myport = int(sys.argv[3])
    else:
        if brokerhost == myhost:
            myport = brokerport + 1
        else:
            myport = 1883

    print("Listening on port", str(myport)+", broker on port", brokerport)
    s = ThreadingTCPServer(("127.0.0.1", myport), MyHandler)
    s.serve_forever()


if __name__ == "__main__":
    run()
