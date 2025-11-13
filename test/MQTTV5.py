"""
*******************************************************************
  Copyright (c) 2013, 2018 IBM Corp.

  All rights reserved. This program and the accompanying materials
  are made available under the terms of the Eclipse Public License v2.0
  and Eclipse Distribution License v1.0 which accompany this distribution.

  The Eclipse Public License is available at
     https://www.eclipse.org/legal/epl-2.0/
  and the Eclipse Distribution License is available at
    http://www.eclipse.org/org/documents/edl-v10.php.

  Contributors:
     Ian Craggs - initial implementation and/or documentation
     Ian Craggs - take MQTT 3.1.1 and create MQTT 5.0 version
*******************************************************************
"""

"""

Assertions are used to validate incoming data, but are omitted from outgoing packets.  This is
so that the tests that use this package can send invalid data for error testing.

"""

import logging, struct

logger = logging.getLogger('MQTTV5')

# Low-level protocol interface

class MQTTException(Exception):
  pass

class MalformedPacket(MQTTException):
  pass

class ProtocolError(MQTTException):
  pass

MAX_PACKET_SIZE = 2**28-1
MAX_PACKETID = 2**16-1

class PacketTypes:

  indexes = range(1, 16)

  # Packet types
  CONNECT, CONNACK, PUBLISH, PUBACK, PUBREC, PUBREL, \
  PUBCOMP, SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK, \
  PINGREQ, PINGRESP, DISCONNECT, AUTH = indexes

  # Dummy packet type for properties use - will delay only applies to will
  WILLMESSAGE = 99


class Packets(object):

  Names = [ "reserved", \
    "Connect", "Connack", "Publish", "Puback", "Pubrec", "Pubrel", \
    "Pubcomp", "Subscribe", "Suback", "Unsubscribe", "Unsuback", \
    "Pingreq", "Pingresp", "Disconnect", "Auth"]

  classNames = [name+'es' if name == "Publish" else
                name+'s' if name != "reserved" else name for name in Names]

  def pack(self):
    buffer = self.fh.pack(0)
    return buffer

  def __str__(self):
    return str(self.fh)

  def __eq__(self, packet):
    return self.fh == packet.fh if packet else False

  def __setattr__(self, name, value):
    if name not in self.names:
      raise MQTTException(name + " Attribute name must be one of "+str(self.names))
    object.__setattr__(self, name, value)


def PacketType(byte):
  """
    Retrieve the message type from the first byte of the fixed header.
  """
  if byte != None:
    rc = byte[0] >> 4
  else:
    rc = None
  return rc

class ReasonCodes:
  """
    The reason code used in MQTT V5.0

  """

  def __getName__(self, packetType, identifier):
    """
    used when displaying the reason code
    """
    assert identifier in self.names.keys(), identifier
    names = self.names[identifier]
    namelist = [name for name in names.keys() if packetType in names[name]]
    assert len(namelist) == 1
    return namelist[0]

  def getId(self, name):
    """
    used when setting the reason code for a packetType
    check that only valid codes for the packet are set
    """
    identifier = None
    for code in self.names.keys():
      if name in self.names[code].keys():
        if self.packetType in self.names[code][name]:
          identifier = code
        break
    assert identifier != None, name
    return identifier

  def set(self, name):
    self.value = self.getId(name)

  def unpack(self, buffer):
    name = self.__getName__(self.packetType, buffer[0])
    self.value = self.getId(name)
    return 1

  def getName(self):
    return self.__getName__(self.packetType, self.value)

  def __str__(self):
    return self.getName()

  def pack(self):
    return bytes([self.value])

  def __init__(self, packetType, aName="Success", identifier=-1):
    self.packetType = packetType
    self.names = {
    0 : { "Success" : [PacketTypes.CONNACK, PacketTypes.PUBACK,
        PacketTypes.PUBREC, PacketTypes.PUBREL, PacketTypes.PUBCOMP,
        PacketTypes.UNSUBACK, PacketTypes.AUTH],
          "Normal disconnection" : [PacketTypes.DISCONNECT],
          "Granted QoS 0" : [PacketTypes.SUBACK] },
    1 : { "Granted QoS 1" : [PacketTypes.SUBACK] },
    2 : { "Granted QoS 2" : [PacketTypes.SUBACK] },
    4 : { "Disconnect with will message" : [PacketTypes.DISCONNECT] },
    16 : { "No matching subscribers" :
      [PacketTypes.PUBACK, PacketTypes.PUBREC] },
    17 : { "No subscription found" : [PacketTypes.UNSUBACK] },
    24 : { "Continue authentication" : [PacketTypes.AUTH] },
    25 : { "Re-authenticate" : [PacketTypes.AUTH] },
    128 : { "Unspecified error" : [PacketTypes.CONNACK, PacketTypes.PUBACK,
      PacketTypes.PUBREC, PacketTypes.SUBACK, PacketTypes.UNSUBACK,
      PacketTypes.DISCONNECT], },
    129 : { "Malformed packet" :
          [PacketTypes.CONNACK, PacketTypes.DISCONNECT] },
    130 : { "Protocol error" :
          [PacketTypes.CONNACK, PacketTypes.DISCONNECT] },
    131 : { "Implementation specific error": [PacketTypes.CONNACK,
          PacketTypes.PUBACK, PacketTypes.PUBREC, PacketTypes.SUBACK,
          PacketTypes.UNSUBACK, PacketTypes.DISCONNECT], },
    132 : { "Unsupported protocol version" : [PacketTypes.CONNACK] },
    133 : { "Client identifier not valid" : [PacketTypes.CONNACK] },
    134 : { "Bad user name or password" : [PacketTypes.CONNACK] },
    135 : { "Not authorized" : [PacketTypes.CONNACK, PacketTypes.PUBACK,
              PacketTypes.PUBREC, PacketTypes.SUBACK, PacketTypes.UNSUBACK,
              PacketTypes.DISCONNECT], },
    136 : { "Server unavailable" : [PacketTypes.CONNACK] },
    137 : { "Server busy" : [PacketTypes.CONNACK, PacketTypes.DISCONNECT] },
    138 : { "Banned" : [PacketTypes.CONNACK] },
    139 : { "Server shutting down" : [PacketTypes.DISCONNECT] },
    140 : { "Bad authentication method" :
            [PacketTypes.CONNACK, PacketTypes.DISCONNECT] },
    141 : { "Keep alive timeout" : [PacketTypes.DISCONNECT] },
    142 : { "Session taken over" : [PacketTypes.DISCONNECT] },
    143 : { "Topic filter invalid" :
            [PacketTypes.SUBACK, PacketTypes.UNSUBACK, PacketTypes.DISCONNECT]},
    144 : { "Topic name invalid" :
            [PacketTypes.CONNACK, PacketTypes.PUBACK,
            PacketTypes.PUBREC, PacketTypes.DISCONNECT]},
    145 : { "Packet identifier in use" :
            [PacketTypes.PUBACK, PacketTypes.PUBREC,
             PacketTypes.SUBACK, PacketTypes.UNSUBACK]},
    146 : { "Packet identifier not found" :
            [PacketTypes.PUBREL, PacketTypes.PUBCOMP] },
    147 : { "Receive maximum exceeded": [PacketTypes.DISCONNECT] },
    148 : { "Topic alias invalid": [PacketTypes.DISCONNECT] },
    149 : { "Packet too large": [PacketTypes.CONNACK, PacketTypes.DISCONNECT] },
    150 : { "Message rate too high": [PacketTypes.DISCONNECT] },
    151 : { "Quota exceeded": [PacketTypes.CONNACK, PacketTypes.PUBACK,
          PacketTypes.PUBREC, PacketTypes.SUBACK, PacketTypes.DISCONNECT], },
    152 : { "Administrative action" : [PacketTypes.DISCONNECT] },
    153 : { "Payload format invalid" :
            [PacketTypes.PUBACK, PacketTypes.PUBREC, PacketTypes.DISCONNECT]},
    154 : { "Retain not supported" :
            [PacketTypes.CONNACK, PacketTypes.DISCONNECT] },
    155 : { "QoS not supported" :
            [PacketTypes.CONNACK, PacketTypes.DISCONNECT] },
    156 : { "Use another server" :
            [PacketTypes.CONNACK, PacketTypes.DISCONNECT] },
    157 : { "Server moved" :
            [PacketTypes.CONNACK, PacketTypes.DISCONNECT] },
    158 : { "Shared subscription not supported" :
            [PacketTypes.SUBACK, PacketTypes.DISCONNECT] },
    159 : { "Connection rate exceeded" :
            [PacketTypes.CONNACK, PacketTypes.DISCONNECT] },
    160 : { "Maximum connect time" :
            [PacketTypes.DISCONNECT] },
    161 : { "Subscription identifiers not supported" :
            [PacketTypes.SUBACK, PacketTypes.DISCONNECT] },
    162 : { "Wildcard subscription not supported" :
            [PacketTypes.SUBACK, PacketTypes.DISCONNECT] },
    }
    if identifier == -1:
      self.set(aName)
    else:
      self.value = identifier
      self.getName() # check it's good


class VBIs:  # Variable Byte Integer

  @staticmethod
  def encode(x):
    """
      Convert an integer 0 <= x <= 268435455 into multi-byte format.
      Returns the buffer convered from the integer.
    """
    assert 0 <= x <= 268435455
    buffer = b''
    while 1:
      digit = x % 128
      x //= 128
      if x > 0:
        digit |= 0x80
      buffer += bytes([digit])
      if x == 0:
        break
    return buffer

  @staticmethod
  def decode(buffer):
    """
      Get the value of a multi-byte integer from a buffer
      Return the value, and the number of bytes used.

      [MQTT-1.5.5-1] the encoded value MUST use the minimum number of bytes necessary to represent the value
    """
    multiplier = 1
    value = 0
    bytes = 0
    while 1:
      bytes += 1
      digit = buffer[0]
      buffer = buffer[1:]
      value += (digit & 127) * multiplier
      if digit & 128 == 0:
        break
      multiplier *= 128
    return (value, bytes)

def getPacket(aSocket):
  "receive the next packet"
  buf = aSocket.recv(1) # get the first byte fixed header
  if buf == b"":
    return None
  if str(aSocket).find("[closed]") != -1:
    closed = True
  else:
    closed = False
  if closed:
    return None
  # now get the remaining length
  multiplier = 1
  remlength = 0
  while 1:
    next = aSocket.recv(1)
    while len(next) == 0:
      next = aSocket.recv(1)
    buf += next
    digit = buf[-1]
    remlength += (digit & 127) * multiplier
    if digit & 128 == 0:
      break
    multiplier *= 128
  # receive the remaining length if there is any
  rest = bytes([])
  if remlength > 0:
    while len(rest) < remlength:
      rest += aSocket.recv(remlength-len(rest))
  assert len(rest) == remlength
  return buf + rest


class FixedHeaders(object):

  def __init__(self, aPacketType):
    self.PacketType = aPacketType
    self.DUP = False
    self.QoS = 0
    self.RETAIN = False
    self.remainingLength = 0

  def __eq__(self, fh):
    return self.PacketType == fh.PacketType and \
           self.DUP == fh.DUP and \
           self.QoS == fh.QoS and \
           self.RETAIN == fh.RETAIN # and \
           # self.remainingLength == fh.remainingLength

  def __setattr__(self, name, value):
    names = ["PacketType", "DUP", "QoS", "RETAIN", "remainingLength"]
    if name not in names:
      raise MQTTException(name + " Attribute name must be one of "+str(names))
    object.__setattr__(self, name, value)

  def __str__(self):
    "return printable representation of our data"
    return Packets.classNames[self.PacketType]+'(fh.DUP='+str(self.DUP)+ \
           ", fh.QoS="+str(self.QoS)+", fh.RETAIN="+str(self.RETAIN)

  def pack(self, length):
    "pack data into string buffer ready for transmission down socket"
    buffer = bytes([(self.PacketType << 4) | (self.DUP << 3) |\
                         (self.QoS << 1) | self.RETAIN])
    self.remainingLength = length
    buffer += VBIs.encode(length)
    return buffer

  def unpack(self, buffer, maximumPacketSize):
    "unpack data from string buffer into separate fields"
    b0 = buffer[0]
    self.PacketType = b0 >> 4
    self.DUP = ((b0 >> 3) & 0x01) == 1
    self.QoS = (b0 >> 1) & 0x03
    self.RETAIN = (b0 & 0x01) == 1
    (self.remainingLength, bytes) = VBIs.decode(buffer[1:])
    if self.remainingLength + bytes + 1 > maximumPacketSize:
       raise ProtocolError("Packet too large")
    return bytes + 1 # length of fixed header


def writeInt16(length):
  return bytes([length // 256, length % 256])

def readInt16(buf):
  return buf[0]*256 + buf[1]

def writeInt32(length):
  buffer = [length // 16777216]
  length %= 16777216
  buffer += [length // 65536]
  length %= 65536
  buffer += [length // 256, length % 256]
  return bytes(buffer)

def readInt32(buf):
  return buf[0]*16777216 + buf[1]*65536 + buf[2]*256 + buf[3]

def writeUTF(data):
  # data could be a string, or bytes.  If string, encode into bytes with utf-8
  return writeInt16(len(data)) + (data if type(data) == type(b"") else bytes(data, "utf-8"))

def readUTF(buffer, maxlen):
  if maxlen >= 2:
    length = readInt16(buffer)
  else:
    raise MalformedPacket("Not enough data to read string length")
  maxlen -= 2
  if length > maxlen:
    raise MalformedPacket("Length delimited string too long")
  buf = buffer[2:2+length].decode("utf-8")
  logger.info("[MQTT-4.7.3-2] topic names and filters must not include null")
  zz = buf.find("\x00") # look for null in the UTF string
  if zz != -1:
    raise MalformedPacket("[MQTT-1.5.4-2] Null found in UTF data "+buf)
  for c in range (0xD800, 0xDFFF):
    zz = buf.find(chr(c)) # look for D800-DFFF in the UTF string
    if zz != -1:
      raise MalformedPacket("[MQTT-1.5.4-1] D800-DFFF found in UTF data "+buf)
  if buf.find("\uFEFF") != -1:
    logger.info("[MQTT-1.5.4-3] U+FEFF in UTF string")
  return buf, length+2

def writeBytes(buffer):
  return writeInt16(len(buffer)) + buffer

def readBytes(buffer):
  length = readInt16(buffer)
  return buffer[2:2+length], length+2


class Properties(object):

  def __init__(self, packetType):
    self.packetType = packetType
    self.types = ["Byte", "Two Byte Integer", "Four Byte Integer", "Variable Byte Integer",
       "Binary Data", "UTF-8 Encoded String", "UTF-8 String Pair"]

    self.names = {
      "Payload Format Indicator" : 1,
      "Message Expiry Interval" : 2,
      "Content Type" : 3,
      "Response Topic" : 8,
      "Correlation Data" : 9,
      "Subscription Identifier" : 11,
      "Session Expiry Interval" : 17,
      "Assigned Client Identifier" : 18,
      "Server Keep Alive" : 19,
      "Authentication Method" : 21,
      "Authentication Data" : 22,
      "Request Problem Information" : 23,
      "Will Delay Interval" : 24,
      "Request Response Information" : 25,
      "Response Information" : 26,
      "Server Reference" : 28,
      "Reason String" : 31,
      "Receive Maximum" : 33,
      "Topic Alias Maximum" : 34,
      "Topic Alias" : 35,
      "Maximum QoS" : 36,
      "Retain Available" : 37,
      "User Property List" : 38,
      "Maximum Packet Size" : 39,
      "Wildcard Subscription Available" : 40,
      "Subscription Identifier Available" : 41,
      "Shared Subscription Available" : 42
    }

    self.properties = {
    # id:  type, packets
      1  : (self.types.index("Byte"), [PacketTypes.PUBLISH, PacketTypes.WILLMESSAGE]), # payload format indicator
      2  : (self.types.index("Four Byte Integer"), [PacketTypes.PUBLISH, PacketTypes.WILLMESSAGE]),
      3  : (self.types.index("UTF-8 Encoded String"), [PacketTypes.PUBLISH, PacketTypes.WILLMESSAGE]),
      8  : (self.types.index("UTF-8 Encoded String"), [PacketTypes.PUBLISH, PacketTypes.WILLMESSAGE]),
      9  : (self.types.index("Binary Data"), [PacketTypes.PUBLISH, PacketTypes.WILLMESSAGE]),
      11 : (self.types.index("Variable Byte Integer"),
           [PacketTypes.PUBLISH, PacketTypes.SUBSCRIBE]),
      17 : (self.types.index("Four Byte Integer"),
           [PacketTypes.CONNECT, PacketTypes.CONNACK, PacketTypes.DISCONNECT]),
      18 : (self.types.index("UTF-8 Encoded String"), [PacketTypes.CONNACK]),
      19 : (self.types.index("Two Byte Integer"), [PacketTypes.CONNACK]),
      21 : (self.types.index("UTF-8 Encoded String"),
           [PacketTypes.CONNECT, PacketTypes.CONNACK, PacketTypes.AUTH]),
      22 : (self.types.index("Binary Data"),
           [PacketTypes.CONNECT, PacketTypes.CONNACK, PacketTypes.AUTH]),
      23 : (self.types.index("Byte"),
           [PacketTypes.CONNECT]),
      24 : (self.types.index("Four Byte Integer"), [PacketTypes.WILLMESSAGE]),
      25 : (self.types.index("Byte"), [PacketTypes.CONNECT]),
      26 : (self.types.index("UTF-8 Encoded String"), [PacketTypes.CONNACK]),
      28 : (self.types.index("UTF-8 Encoded String"),
           [PacketTypes.CONNACK, PacketTypes.DISCONNECT]),
      31 : (self.types.index("UTF-8 Encoded String"),
           [PacketTypes.CONNACK, PacketTypes.PUBACK, PacketTypes.PUBREC,
            PacketTypes.PUBREL, PacketTypes.PUBCOMP, PacketTypes.SUBACK,
            PacketTypes.UNSUBACK, PacketTypes.DISCONNECT, PacketTypes.AUTH]),
      33 : (self.types.index("Two Byte Integer"),
           [PacketTypes.CONNECT, PacketTypes.CONNACK]),
      34 : (self.types.index("Two Byte Integer"),
           [PacketTypes.CONNECT, PacketTypes.CONNACK]),
      35 : (self.types.index("Two Byte Integer"), [PacketTypes.PUBLISH]),
      36 : (self.types.index("Byte"), [PacketTypes.CONNACK]),
      37 : (self.types.index("Byte"), [PacketTypes.CONNACK]),
      38 : (self.types.index("UTF-8 String Pair"),
           [PacketTypes.CONNECT, PacketTypes.CONNACK,
           PacketTypes.PUBLISH, PacketTypes.PUBACK,
           PacketTypes.PUBREC, PacketTypes.PUBREL, PacketTypes.PUBCOMP,
           PacketTypes.SUBSCRIBE, PacketTypes.SUBACK,
           PacketTypes.UNSUBSCRIBE, PacketTypes.UNSUBACK,
           PacketTypes.DISCONNECT, PacketTypes.AUTH, PacketTypes.WILLMESSAGE]),
      39 : (self.types.index("Four Byte Integer"),
           [PacketTypes.CONNECT, PacketTypes.CONNACK]),
      40 : (self.types.index("Byte"), [PacketTypes.CONNACK]),
      41 : (self.types.index("Byte"), [PacketTypes.CONNACK]),
      42 : (self.types.index("Byte"), [PacketTypes.CONNACK]),
    }

  def getIdentFromName(self, compressedName):
    # return the identifier corresponding to the property name
    result = -1
    for name in self.names.keys():
      if compressedName == name.replace(' ', ''):
         result = self.names[name]
         break
    return result

  def __setattr__(self, name, value):
    name = name.replace(' ', '')
    privateVars = ["packetType", "types", "names", "properties"]
    if name in privateVars:
      object.__setattr__(self, name, value)
    else:
      # the name could have spaces in, or not.  Remove spaces before assignment
      if name not in [name.replace(' ', '') for name in self.names.keys()]:
        raise MQTTException("Attribute name must be one of "+str(self.names.keys()))
      # check that this attribute applies to the packet type
      if self.packetType not in self.properties[self.getIdentFromName(name)][1]:
        raise MQTTException("Attribute %s does not apply to packet type %s"
            % (name, Packets.Names[self.packetType]) )
      object.__setattr__(self, name, value)

  def __str__(self):
    buffer = "["
    first = True
    for name in self.names.keys():
      compressedName = name.replace(' ', '')
      if hasattr(self, compressedName):
        if not first:
          buffer += ", "
        buffer += compressedName +" : "+str(getattr(self, compressedName))
        first = False
    buffer += "]"
    return buffer

  def isEmpty(self):
    rc = True
    for name in self.names.keys():
      compressedName = name.replace(' ', '')
      if hasattr(self, compressedName):
        rc = False
        break
    return rc

  def clear(self):
    for name in self.names.keys():
      compressedName = name.replace(' ', '')
      if hasattr(self, compressedName):
        delattr(self, compressedName)

  def writeProperty(self, identifier, type, value):
    buffer = b""
    buffer += VBIs.encode(identifier) # identifier
    if type == self.types.index("Byte"): # value
      buffer += bytes([value])
    elif type == self.types.index("Two Byte Integer"):
      buffer += writeInt16(value)
    elif type == self.types.index("Four Byte Integer"):
      buffer += writeInt32(value)
    elif type == self.types.index("Variable Byte Integer"):
      buffer += VBIs.encode(value)
    elif type == self.types.index("Binary Data"):
      buffer += writeBytes(value)
    elif type == self.types.index("UTF-8 Encoded String"):
      buffer += writeUTF(value)
    elif type == self.types.index("UTF-8 String Pair"):
      buffer += writeUTF(value[0]) + writeUTF(value[1])
    return buffer

  def pack(self):
    # serialize properties into buffer for sending over network
    buffer = b""
    for name in self.names.keys():
      compressedName = name.replace(' ', '')
      isList = False
      if compressedName.endswith('List'):
        isList = True
      if hasattr(self, compressedName):
        identifier = self.getIdentFromName(compressedName)
        attr_type = self.properties[identifier][0]
        if isList:
          for prop in getattr(self, compressedName):
            buffer += self.writeProperty(identifier, attr_type, prop)
        else:
          buffer += self.writeProperty(identifier, attr_type,
                           getattr(self, compressedName))
    return VBIs.encode(len(buffer)) + buffer

  def readProperty(self, buffer, type, propslen):
    if type == self.types.index("Byte"):
      value = buffer[0]
      valuelen = 1
    elif type == self.types.index("Two Byte Integer"):
      value = readInt16(buffer)
      valuelen = 2
    elif type == self.types.index("Four Byte Integer"):
      value = readInt32(buffer)
      valuelen = 4
    elif type == self.types.index("Variable Byte Integer"):
      value, valuelen = VBIs.decode(buffer)
    elif type == self.types.index("Binary Data"):
      value, valuelen = readBytes(buffer)
    elif type == self.types.index("UTF-8 Encoded String"):
      value, valuelen = readUTF(buffer, propslen)
    elif type == self.types.index("UTF-8 String Pair"):
      value, valuelen = readUTF(buffer, propslen)
      buffer = buffer[valuelen:] # strip the bytes used by the value
      value1, valuelen1 = readUTF(buffer, propslen - valuelen)
      value = (value, value1)
      valuelen += valuelen1
    return value, valuelen

  def getNameFromIdent(self, identifier):
    rc = None
    for name in self.names:
      if self.names[name] == identifier:
        rc = name
    return rc

  def unpack(self, buffer):
    self.clear()
    # deserialize properties into attributes from buffer received from network
    propslen, VBIlen = VBIs.decode(buffer)
    buffer = buffer[VBIlen:] # strip the bytes used by the VBI
    propslenleft = propslen
    while propslenleft > 0: # properties length is 0 if there are none
      identifier, VBIlen = VBIs.decode(buffer) # property identifier
      buffer = buffer[VBIlen:] # strip the bytes used by the VBI
      propslenleft -= VBIlen
      attr_type = self.properties[identifier][0]
      value, valuelen = self.readProperty(buffer, attr_type, propslenleft)
      buffer = buffer[valuelen:] # strip the bytes used by the value
      propslenleft -= valuelen
      propname = self.getNameFromIdent(identifier)
      compressedName = propname.replace(' ', '')
      if propname.endswith('List'):
        if not hasattr(self, compressedName):
          setattr(self, propname, [value])
        else:
          setattr(self, propname, getattr(self, compressedName) + [value])
      else:
        if hasattr(self, compressedName):
          raise MQTTException("Property '%s' must not exist more than once" % property)
        setattr(self, propname, value)
    return self, propslen + VBIlen


class Connects(Packets):

  def __init__(self, buffer = None):
    object.__setattr__(self, "names",
         ["fh", "properties", "willProperties", "ProtocolName", "ProtocolVersion",
          "ClientIdentifier", "CleanStart", "KeepAliveTimer",
          "WillFlag", "WillQoS", "WillRETAIN", "WillTopic", "WillMessage",
          "usernameFlag", "passwordFlag", "username", "password"])

    self.fh = FixedHeaders(PacketTypes.CONNECT)

    # variable header
    self.ProtocolName = "MQTT"
    self.ProtocolVersion = 5
    self.CleanStart = True
    self.WillFlag = False
    self.WillQoS = 0
    self.WillRETAIN = 0
    self.KeepAliveTimer = 30
    self.usernameFlag = False
    self.passwordFlag = False

    self.properties = Properties(PacketTypes.CONNECT)
    self.willProperties = Properties(PacketTypes.WILLMESSAGE)

    # Payload
    self.ClientIdentifier = ""   # UTF-8
    self.WillTopic = None        # UTF-8
    self.WillMessage = None      # binary
    self.username = None         # UTF-8
    self.password = None         # binary

    #self.properties = Properties()
    if buffer != None:
      self.unpack(buffer)

  def pack(self):
    connectFlags = bytes([(self.CleanStart << 1) | (self.WillFlag << 2) | \
                       (self.WillQoS << 3) | (self.WillRETAIN << 5) | \
                       (self.usernameFlag << 6) | (self.passwordFlag << 7)])
    buffer = writeUTF(self.ProtocolName) + bytes([self.ProtocolVersion]) + \
              connectFlags + writeInt16(self.KeepAliveTimer)
    buffer += self.properties.pack()
    buffer += writeUTF(self.ClientIdentifier)
    if self.WillFlag:
      assert self.willProperties.packetType == PacketTypes.WILLMESSAGE
      buffer += self.willProperties.pack()
      buffer += writeUTF(self.WillTopic)
      buffer += writeBytes(self.WillMessage)
    if self.usernameFlag:
      buffer += writeUTF(self.username)
    if self.passwordFlag:
      buffer += writeBytes(self.password)
    buffer = self.fh.pack(len(buffer)) + buffer
    return buffer

  def unpack(self, buffer, maximumPacketSize):
    assert len(buffer) >= 2
    assert PacketType(buffer) == PacketTypes.CONNECT

    try:
      fhlen = self.fh.unpack(buffer, maximumPacketSize)
      packlen = fhlen + self.fh.remainingLength
      assert len(buffer) >= packlen, "buffer length %d packet length %d" % (len(buffer), packlen)
      curlen = fhlen # points to after header + remaining length
      assert self.fh.DUP == False, "[MQTT-2.1.2-1]"
      assert self.fh.QoS == 0, "[MQTT-2.1.2-1] QoS was not 0, was %d" % self.fh.QoS
      assert self.fh.RETAIN == False, "[MQTT-2.1.2-1]"

      # to allow the server to send back a CONNACK with unsupported protocol version,
      # the following two assertions will need to be disabled
      self.ProtocolName, valuelen = readUTF(buffer[curlen:], packlen - curlen)
      curlen += valuelen
      assert self.ProtocolName == "MQTT", "Wrong protocol name %s" % self.ProtocolName

      self.ProtocolVersion = buffer[curlen]
      curlen += 1
      assert self.ProtocolVersion == 5, "Wrong protocol version %s" % self.ProtocolVersion

      connectFlags = buffer[curlen]
      assert (connectFlags & 0x01) == 0, "[MQTT-3.1.2-3] reserved connect flag must be 0"
      self.CleanStart = ((connectFlags >> 1) & 0x01) == 1
      self.WillFlag = ((connectFlags >> 2) & 0x01) == 1
      self.WillQoS = (connectFlags >> 3) & 0x03
      self.WillRETAIN = (connectFlags >> 5) & 0x01
      self.passwordFlag = ((connectFlags >> 6) & 0x01) == 1
      self.usernameFlag = ((connectFlags >> 7) & 0x01) == 1
      curlen += 1

      if self.WillFlag:
        assert self.WillQoS in [0, 1, 2], "[MQTT-3.1.2-12] will qos must not be 3"
      else:
        assert self.WillQoS == 0, "[MQTT-3.1.2-11] will qos must be 0, if will flag is false"
        assert self.WillRETAIN == False, "[MQTT-3.1.2-13] will retain must be false, if will flag is false"

      self.KeepAliveTimer = readInt16(buffer[curlen:])
      curlen += 2

      curlen += self.properties.unpack(buffer[curlen:])[1]

      logger.info("[MQTT-3.1.3-3] Clientid must be present, and first field")
      logger.info("[MQTT-3.1.3-4] Clientid must be Unicode, and between 0 and 65535 bytes long")
      self.ClientIdentifier, valuelen = readUTF(buffer[curlen:], packlen - curlen)
      curlen += valuelen

      if self.WillFlag:
        curlen += self.willProperties.unpack(buffer[curlen:])[1]
        self.WillTopic, valuelen = readUTF(buffer[curlen:], packlen - curlen)
        curlen += valuelen
        self.WillMessage, valuelen = readBytes(buffer[curlen:])
        curlen += valuelen
        logger.info("[[MQTT-3.1.2-9] will topic and will message fields must be present")
      else:
        self.WillTopic = self.WillMessage = None

      if self.usernameFlag:
        assert len(buffer) > curlen+2, "Buffer too short to read username length"
        self.username, valuelen = readUTF(buffer[curlen:], packlen - curlen)
        curlen += valuelen
        logger.info("[MQTT-3.1.2-19] username must be in payload if user name flag is 1")
      else:
        logger.info("[MQTT-3.1.2-18] username must not be in payload if user name flag is 0")
        assert self.passwordFlag == False, "[MQTT-3.1.2-22] password flag must be 0 if username flag is 0"

      if self.passwordFlag:
        assert len(buffer) > curlen+2, "Buffer too short to read password length"
        self.password, valuelen = readBytes(buffer[curlen:])
        curlen += valuelen
        logger.info("[MQTT-3.1.2-21] password must be in payload if password flag is 0")
      else:
        logger.info("[MQTT-3.1.2-20] password must not be in payload if password flag is 0")

      if self.WillFlag and self.usernameFlag and self.passwordFlag:
        logger.info("[MQTT-3.1.3-1] clientid, will topic, will message, username and password all present")

      assert curlen == packlen, "Packet is wrong length curlen %d != packlen %d" % (curlen, packlen)
    except:
      logger.exception("[MQTT-3.1.4-1] server must validate connect packet and close connection without connack if it does not conform")
      raise

  def __str__(self):
    buf = str(self.fh)+", ProtocolName="+str(self.ProtocolName)+", ProtocolVersion=" +\
          str(self.ProtocolVersion)+", CleanStart="+str(self.CleanStart) +\
          ", WillFlag="+str(self.WillFlag)+", KeepAliveTimer=" +\
          str(self.KeepAliveTimer)+", ClientId="+str(self.ClientIdentifier) +\
          ", usernameFlag="+str(self.usernameFlag)+", passwordFlag="+str(self.passwordFlag)
    if self.WillFlag:
      buf += ", WillQoS=" + str(self.WillQoS) +\
             ", WillRETAIN=" + str(self.WillRETAIN) +\
             ", WillTopic='"+ self.WillTopic +\
             "', WillMessage='"+str(self.WillMessage)+"'"
    if self.username:
      buf += ", username="+self.username
    if self.password:
      buf += ", password="+str(self.password)
    buf += ", properties="+str(self.properties)
    return buf+")"

  def __eq__(self, packet):
    rc = Packets.__eq__(self, packet) and \
           self.ProtocolName == packet.ProtocolName and \
           self.ProtocolVersion == packet.ProtocolVersion and \
           self.CleanStart == packet.CleanStart and \
           self.WillFlag == packet.WillFlag and \
           self.KeepAliveTimer == packet.KeepAliveTimer and \
           self.ClientIdentifier == packet.ClientIdentifier and \
           self.WillFlag == packet.WillFlag
    if rc and self.WillFlag:
      rc = self.WillQoS == packet.WillQoS and \
           self.WillRETAIN == packet.WillRETAIN and \
           self.WillTopic == packet.WillTopic and \
           self.WillMessage == packet.WillMessage
    if rc:
      rc = self.properties == packet.properties
    return rc


class Connacks(Packets):

  def __init__(self, buffer=None, DUP=False, QoS=0, RETAIN=False, ReasonCode="Success"):
    object.__setattr__(self, "names",
         ["fh", "sessionPresent", "reasonCode", "properties"])
    self.fh = FixedHeaders(PacketTypes.CONNACK)
    self.fh.DUP = DUP
    self.fh.QoS = QoS
    self.fh.RETAIN = RETAIN
    self.sessionPresent = False
    self.reasonCode = ReasonCodes(PacketTypes.CONNACK, ReasonCode)
    self.properties = Properties(PacketTypes.CONNACK)
    if buffer != None:
      self.unpack(buffer)

  def pack(self):
    flags = 0x01 if self.sessionPresent else 0x00
    buffer = bytes([flags])
    buffer += self.reasonCode.pack()
    buffer += self.properties.pack()
    buffer = self.fh.pack(len(buffer)) + buffer
    return buffer

  def unpack(self, buffer, maximumPacketSize):
    assert len(buffer) >= 4
    assert PacketType(buffer) == PacketTypes.CONNACK
    curlen = self.fh.unpack(buffer, maximumPacketSize)
    assert buffer[curlen] in [0, 1], "Connect Acknowledge Flags"
    self.sessionPresent = (buffer[curlen] == 0x01)
    curlen += 1
    curlen += self.reasonCode.unpack(buffer[curlen:])
    curlen += self.properties.unpack(buffer[curlen:])[1]
    assert self.fh.DUP == False, "[MQTT-2.1.2-1]"
    assert self.fh.QoS == 0, "[MQTT-2.1.2-1]"
    assert self.fh.RETAIN == False, "[MQTT-2.1.2-1]"

  def __str__(self):
    return str(self.fh)+", Session present="+str((self.sessionPresent & 0x01) == 1)+\
          ", ReturnCode="+str(self.reasonCode)+\
          ", properties="+str(self.properties)+")"

  def __eq__(self, packet):
    return Packets.__eq__(self, packet) and \
           self.reasonCode == packet.reasonCode


class Disconnects(Packets):

  def __init__(self, buffer=None, DUP=False, QoS=0, RETAIN=False,
          reasonCode="Normal disconnection"):
    object.__setattr__(self, "names",
        ["fh", "DUP", "QoS", "RETAIN", "reasonCode", "properties"])
    self.fh = FixedHeaders(PacketTypes.DISCONNECT)
    self.fh.DUP = DUP
    self.fh.QoS = QoS
    self.fh.RETAIN = RETAIN
    # variable header
    self.reasonCode = ReasonCodes(PacketTypes.DISCONNECT, identifier=reasonCode)
    self.properties = Properties(PacketTypes.DISCONNECT)
    if buffer != None:
      self.unpack(buffer)

  def pack(self):
    buffer = b""
    if self.reasonCode.getName() != "Normal disconnection" or not self.properties.isEmpty():
      buffer += self.reasonCode.pack()
      if not self.properties.isEmpty():
        buffer += self.properties.pack()
    buffer = self.fh.pack(len(buffer)) + buffer
    return buffer

  def unpack(self, buffer, maximumPacketSize):
    self.properties.clear()
    self.reasonCode.set("Normal disconnection")
    assert len(buffer) >= 2
    assert PacketType(buffer) == PacketTypes.DISCONNECT
    fhlen = self.fh.unpack(buffer, maximumPacketSize)
    assert len(buffer) >= fhlen + self.fh.remainingLength
    assert self.fh.DUP == False, "[MQTT-2.1.2-1] DISCONNECT reserved bits must be 0"
    assert self.fh.QoS == 0, "[MQTT-2.1.2-1] DISCONNECT reserved bits must be 0"
    assert self.fh.RETAIN == False, "[MQTT-2.1.2-1] DISCONNECT reserved bits must be 0"
    curlen = 0
    if self.fh.remainingLength > 0:
      self.reasonCode.unpack(buffer[curlen:])
      curlen += 1
    if self.fh.remainingLength > 1:
      curlen += self.properties.unpack(buffer[curlen:])[1]
    assert curlen == self.fh.remainingLength, \
            "DISCONNECT packet is wrong length %d" % self.fh.remainingLength
    return fhlen + self.fh.remainingLength

  def __str__(self):
    return str(self.fh)+", ReasonCode: "+str(self.reasonCode)+", Properties: "+str(self.properties)

  def __eq__(self, packet):
    return Packets.__eq__(self, packet) and \
           self.reasonCode == packet.reasonCode and \
           self.properties == packet.properties


class Publishes(Packets):

  def __init__(self, buffer=None, DUP=False, QoS=0, RETAIN=False, MsgId=1, TopicName="", Payload=b""):
    object.__setattr__(self, "names",
          ["fh", "DUP", "QoS", "RETAIN", "topicName", "packetIdentifier",
           "properties", "data", "qos2state", "receivedTime"])
    self.fh = FixedHeaders(PacketTypes.PUBLISH)
    self.fh.DUP = DUP
    self.fh.QoS = QoS
    self.fh.RETAIN = RETAIN
    # variable header
    self.topicName = TopicName
    self.packetIdentifier = MsgId
    self.properties = Properties(PacketTypes.PUBLISH)
    # payload
    self.data = Payload
    if buffer != None:
      self.unpack(buffer)

  def pack(self):
    buffer = writeUTF(self.topicName)
    if self.fh.QoS != 0:
      buffer +=  writeInt16(self.packetIdentifier)
    buffer += self.properties.pack()
    buffer += self.data
    buffer = self.fh.pack(len(buffer)) + buffer
    return buffer

  def unpack(self, buffer, maximumPacketSize):
    assert len(buffer) >= 2
    assert PacketType(buffer) == PacketTypes.PUBLISH
    fhlen = self.fh.unpack(buffer, maximumPacketSize)
    assert self.fh.QoS in [0, 1, 2], "QoS in Publish must be 0, 1, or 2"
    packlen = fhlen + self.fh.remainingLength
    assert len(buffer) >= packlen
    curlen = fhlen
    try:
      self.topicName, valuelen = readUTF(buffer[fhlen:], packlen - curlen)
    except UnicodeDecodeError:
      logger.info("[MQTT-3.3.2-1] topic name in publish must be utf-8")
      raise
    curlen += valuelen
    if self.fh.QoS != 0:
      self.packetIdentifier = readInt16(buffer[curlen:])
      logger.info("[MQTT-2.3.1-1] packet indentifier must be in publish if QoS is 1 or 2")
      curlen += 2
      assert self.packetIdentifier > 0, "[MQTT-2.3.1-1] packet indentifier must be > 0"
    else:
      logger.info("[MQTT-2.3.1-5] no packet indentifier in publish if QoS is 0")
      self.packetIdentifier = 0
    curlen += self.properties.unpack(buffer[curlen:])[1]
    self.data = buffer[curlen:fhlen + self.fh.remainingLength]
    if self.fh.QoS == 0:
      assert self.fh.DUP == False, "[MQTT-2.1.2-4]"
    return fhlen + self.fh.remainingLength

  def __str__(self):
    rc = str(self.fh)
    if self.fh.QoS != 0:
      rc += ", PacketId="+str(self.packetIdentifier)
    rc += ", Properties: "+str(self.properties)
    rc += ", TopicName="+str(self.topicName)+", Payload="+str(self.data)+")"
    return rc

  def __eq__(self, packet):
    rc = Packets.__eq__(self, packet) and \
         self.topicName == packet.topicName and \
         self.data == packet.data
    if rc and self.fh.QoS != 0:
      rc = self.packetIdentifier == packet.packetIdentifier
    return rc


class Acks(Packets):

  def __init__(self, ackType, buffer, DUP, QoS, RETAIN, packetId):
    object.__setattr__(self, "names",
        ["fh", "DUP", "QoS", "RETAIN", "packetIdentifier",
         "reasonCode", "properties"])
    self.fh = FixedHeaders(ackType)
    self.fh.DUP = DUP
    self.fh.QoS = QoS
    self.fh.RETAIN = RETAIN
    # variable header
    self.packetIdentifier = packetId
    self.reasonCode = ReasonCodes(ackType)
    self.properties = Properties(ackType)
    object.__setattr__(self, "ackType", ackType)
    object.__setattr__(self, "ackName", Packets.Names[self.ackType])
    if buffer != None:
      self.unpack(buffer)

  def pack(self):
    buffer = writeInt16(self.packetIdentifier)
    if self.reasonCode.getName() != "Success" or not self.properties.isEmpty():
      buffer += self.reasonCode.pack()
      if not self.properties.isEmpty():
        buffer += self.properties.pack()
    buffer = self.fh.pack(len(buffer)) + buffer
    return buffer

  def unpack(self, buffer, maximumPacketSize):
    self.properties.clear()
    self.reasonCode.set("Success")
    assert len(buffer) >= 2
    assert PacketType(buffer) == self.ackType
    fhlen = self.fh.unpack(buffer, maximumPacketSize)
    assert self.fh.remainingLength in [2, 3, 4], \
        "%s packet is wrong length %d" % (self.ackName, self.fh.remainingLength)
    assert len(buffer) >= fhlen + self.fh.remainingLength
    self.packetIdentifier = readInt16(buffer[fhlen:])
    curlen = fhlen + 2
    assert self.fh.DUP == False, "[MQTT-2.1.2-1] %s reserved bits must be 0" %\
      self.ackName
    if self.ackType == PacketTypes.PUBREL:
      assert self.fh.QoS == 1, "[MQTT-3.6.1-1] %s reserved bits must be 0010" %\
        self.ackName
    else:
      assert self.fh.QoS == 0, "[MQTT-2.1.2-1] %s reserved bits must be 0" %\
        self.ackName
    assert self.fh.RETAIN == False, "[MQTT-2.1.2-1] %s reserved bits must be 0" %\
      self.ackName
    if self.fh.remainingLength > 2:
      self.reasonCode.unpack(buffer[curlen:])
      curlen += 1
    if self.fh.remainingLength > 3:
      self.properties.unpack(buffer[curlen:])
    return fhlen + self.fh.remainingLength

  def __str__(self):
    return str(self.fh)+", PacketId="+str(self.packetIdentifier)+")"

  def __eq__(self, packet):
    return Packets.__eq__(self, packet) and \
           self.packetIdentifier == packet.packetIdentifier


class Pubacks(Acks):

  def __init__(self, buffer=None, DUP=False, QoS=0, RETAIN=False, PacketId=1):
    Acks.__init__(self, PacketTypes.PUBACK, buffer, DUP, QoS, RETAIN, PacketId)

class Pubrecs(Acks):

  def __init__(self, buffer=None, DUP=False, QoS=0, RETAIN=False, PacketId=1):
    Acks.__init__(self, PacketTypes.PUBREC, buffer, DUP, QoS, RETAIN, PacketId)

class Pubrels(Acks):

  def __init__(self, buffer=None, DUP=False, QoS=1, RETAIN=False, PacketId=1):
    Acks.__init__(self, PacketTypes.PUBREL, buffer, DUP, QoS, RETAIN, PacketId)

class Pubcomps(Acks):

  def __init__(self, buffer=None, DUP=False, QoS=0, RETAIN=False, PacketId=1):
    Acks.__init__(self, PacketTypes.PUBCOMP, buffer, DUP, QoS, RETAIN, PacketId)

class SubscribeOptions(object):

  def __init__(self, QoS=0, noLocal=False, retainAsPublished=False, retainHandling=0):
    object.__setattr__(self, "names",
           ["QoS", "noLocal", "retainAsPublished", "retainHandling"])
    self.QoS = QoS # bits 0,1
    self.noLocal = noLocal # bit 2
    self.retainAsPublished = retainAsPublished # bit 3
    self.retainHandling = retainHandling # bits 4 and 5: 0, 1 or 2

  def __setattr__(self, name, value):
    if name not in self.names:
      raise MQTTException(name + " Attribute name must be one of "+str(self.names))
    object.__setattr__(self, name, value)

  def pack(self):
    assert self.QoS in [0, 1, 2]
    assert self.retainHandling in [0, 1, 2]
    noLocal = 1 if self.noLocal else 0
    retainAsPublished = 1 if self.retainAsPublished else 0
    buffer = bytes([(self.retainHandling << 4) | (retainAsPublished << 3) |\
                         (noLocal << 2) | self.QoS])
    return buffer

  def unpack(self, buffer):
    b0 = buffer[0]
    self.retainHandling = ((b0 >> 4) & 0x03)
    self.retainAsPublished = True if ((b0 >> 3) & 0x01) == 1 else False
    self.noLocal = True if ((b0 >> 2) & 0x01) == 1 else False
    self.QoS = (b0 & 0x03)
    assert self.retainHandling in [0, 1, 2]
    assert self.QoS in [0, 1, 2]
    return 1

  def __str__(self):
    return "{QoS="+str(self.QoS)+", noLocal="+str(self.noLocal)+\
        ", retainAsPublished="+str(self.retainAsPublished)+\
        ", retainHandling="+str(self.retainHandling)+"}"


class Subscribes(Packets):

  def __init__(self, buffer=None, DUP=False, QoS=1, RETAIN=False, MsgId=1, Data=[]):
    object.__setattr__(self, "names",
       ["fh", "DUP", "QoS", "RETAIN", "packetIdentifier",
        "properties", "data"])
    self.fh = FixedHeaders(PacketTypes.SUBSCRIBE)
    self.fh.DUP = DUP
    self.fh.QoS = QoS
    self.fh.RETAIN = RETAIN
    # variable header
    self.packetIdentifier = MsgId
    self.properties = Properties(PacketTypes.SUBSCRIBE)
    # payload - list of topic, subscribe option pairs
    self.data = Data[:]
    if buffer != None:
      self.unpack(buffer)

  def pack(self):
    buffer = writeInt16(self.packetIdentifier)
    buffer += self.properties.pack()
    for d in self.data:
      buffer += writeUTF(d[0]) + d[1].pack()
    buffer = self.fh.pack(len(buffer)) + buffer
    return buffer

  def unpack(self, buffer, maximumPacketSize):
    self.properties.clear()
    assert len(buffer) >= 2
    assert PacketType(buffer) == PacketTypes.SUBSCRIBE
    fhlen = self.fh.unpack(buffer, maximumPacketSize)
    assert len(buffer) >= fhlen + self.fh.remainingLength
    logger.info("[MQTT-2.3.1-1] packet indentifier must be in subscribe")
    self.packetIdentifier = readInt16(buffer[fhlen:])
    assert self.packetIdentifier > 0, "[MQTT-2.3.1-1] packet indentifier must be > 0"
    leftlen = self.fh.remainingLength - 2
    leftlen -= self.properties.unpack(buffer[-leftlen:])[1]
    self.data = []
    while leftlen > 0:
      topic, topiclen = readUTF(buffer[-leftlen:], leftlen)
      leftlen -= topiclen
      options = SubscribeOptions()
      options.unpack(buffer[-leftlen:])
      leftlen -= 1
      self.data.append((topic, options))
    assert len(self.data) > 0, "[MQTT-3.8.3-1] at least one topic, qos pair must be in subscribe"
    assert leftlen == 0
    assert self.fh.DUP == False, "[MQTT-2.1.2-1] DUP must be false in subscribe"
    assert self.fh.QoS == 1, "[MQTT-2.1.2-1] QoS must be 1 in subscribe"
    assert self.fh.RETAIN == False, "[MQTT-2.1.2-1] RETAIN must be false in subscribe"
    return fhlen + self.fh.remainingLength

  def __str__(self):
    return str(self.fh)+", PacketId="+str(self.packetIdentifier)+\
           ", Properties: "+str(self.properties)+\
           ", Data="+str( [(x, str(y)) for (x, y) in self.data] ) +")"

  def __eq__(self, packet):
    return Packets.__eq__(self, packet) and \
           self.packetIdentifier == packet.packetIdentifier and \
           self.data == packet.data


class UnsubSubacks(Packets):

  def __init__(self, packetType, buffer, DUP, QoS, RETAIN, PacketId, reasonCodes):
    object.__setattr__(self, "names",
      ["fh", "DUP", "QoS", "RETAIN", "packetIdentifier",
       "reasonCodes", "properties"])
    object.__setattr__(self, "packetType", packetType)
    self.fh = FixedHeaders(self.packetType)
    self.fh.DUP = DUP
    self.fh.QoS = QoS
    self.fh.RETAIN = RETAIN
    # variable header
    self.packetIdentifier = PacketId
    self.properties = Properties(self.packetType)
    # payload - list of reason codes corresponding to topics in subscribe
    self.reasonCodes = reasonCodes[:]
    if buffer != None:
      self.unpack(buffer)

  def pack(self):
    buffer = writeInt16(self.packetIdentifier)
    buffer += self.properties.pack()
    for reasonCode in self.reasonCodes:
      buffer += reasonCode.pack()
    buffer = self.fh.pack(len(buffer)) + buffer
    assert len(buffer) >= 3 # must have property field, even if empty
    return buffer

  def unpack(self, buffer, maximumPacketSize):
    assert len(buffer) >= 3
    assert PacketType(buffer) == self.packetType
    fhlen = self.fh.unpack(buffer, maximumPacketSize)
    assert len(buffer) >= fhlen + self.fh.remainingLength
    self.packetIdentifier = readInt16(buffer[fhlen:])
    leftlen = self.fh.remainingLength - 2
    leftlen -= self.properties.unpack(buffer[-leftlen:])[1]
    self.reasonCodes = []
    while leftlen > 0:
      if self.packetType == PacketTypes.SUBACK:
        reasonCode = ReasonCodes(self.packetType, "Granted QoS 0")
      else:
        reasonCode = ReasonCodes(self.packetType, "Success")
      reasonCode.unpack(buffer[-leftlen:])
      assert reasonCode.value in [0, 1, 2, 0x80], "[MQTT-3.9.3-2] return code in QoS must be 0, 1, 2 or 0x80"
      leftlen -= 1
      self.reasonCodes.append(reasonCode)
    assert leftlen == 0
    assert self.fh.DUP == False, "[MQTT-2.1.2-1] DUP should be false in suback"
    assert self.fh.QoS == 0, "[MQTT-2.1.2-1] QoS should be 0 in suback"
    assert self.fh.RETAIN == False, "[MQTT-2.1.2-1] Retain should be false in suback"
    return fhlen + self.fh.remainingLength

  def __str__(self):
    return str(self.fh)+", PacketId="+str(self.packetIdentifier)+\
           ", Properties: "+str(self.properties)+\
           ", reason codes="+str([str(rc) for rc in self.reasonCodes])+")"

  def __eq__(self, packet):
    return Packets.__eq__(self, packet) and \
           self.packetIdentifier == packet.packetIdentifier and \
           self.data == packet.data


class Subacks(UnsubSubacks):

  def __init__(self, buffer=None, DUP=False, QoS=0, RETAIN=False, PacketId=1, reasonCodes=[]):
      UnsubSubacks.__init__(self, PacketTypes.SUBACK, buffer, DUP, QoS, RETAIN, PacketId, reasonCodes)


class Unsubscribes(Packets):

  def __init__(self, buffer=None, DUP=False, QoS=1, RETAIN=False, PacketId=1, TopicFilters=[]):
    object.__setattr__(self, "names",
       ["fh", "DUP", "QoS", "RETAIN", "packetIdentifier", "properties", "topicFilters"])
    self.fh = FixedHeaders(PacketTypes.UNSUBSCRIBE)
    self.fh.DUP = DUP
    self.fh.QoS = QoS
    self.fh.RETAIN = RETAIN
    # variable header
    self.packetIdentifier = PacketId
    self.properties = Properties(PacketTypes.UNSUBSCRIBE)
    # payload - list of topics
    self.topicFilters = TopicFilters[:]
    if buffer != None:
      self.unpack(buffer)

  def pack(self):
    buffer = writeInt16(self.packetIdentifier)
    buffer += self.properties.pack()
    for topicFilter in self.topicFilters:
      buffer += writeUTF(topicFilter)
    buffer = self.fh.pack(len(buffer)) + buffer
    return buffer

  def unpack(self, buffer, maximumPacketSize):
    assert len(buffer) >= 2
    assert PacketType(buffer) == PacketTypes.UNSUBSCRIBE
    fhlen = self.fh.unpack(buffer, maximumPacketSize)
    assert len(buffer) >= fhlen + self.fh.remainingLength
    logger.info("[MQTT-2.3.1-1] packet indentifier must be in unsubscribe")
    self.packetIdentifier = readInt16(buffer[fhlen:])
    assert self.packetIdentifier > 0, "[MQTT-2.3.1-1] packet indentifier must be > 0"
    leftlen = self.fh.remainingLength - 2
    leftlen -= self.properties.unpack(buffer[-leftlen:])[1]
    self.topicFilters = []
    while leftlen > 0:
      topic, topiclen = readUTF(buffer[-leftlen:], leftlen)
      leftlen -= topiclen
      self.topicFilters.append(topic)
    assert leftlen == 0
    assert self.fh.DUP == False, "[MQTT-2.1.2-1]"
    assert self.fh.QoS == 1, "[MQTT-2.1.2-1]"
    assert self.fh.RETAIN == False, "[MQTT-2.1.2-1]"
    logger.info("[MQTT-3-10.1-1] fixed header bits are 0,0,1,0")
    return fhlen + self.fh.remainingLength

  def __str__(self):
    return str(self.fh)+", PacketId="+str(self.packetIdentifier)+\
           ", Properties: "+str(self.properties)+\
           ", Data="+str(self.topicFilters)+")"

  def __eq__(self, packet):
    return Packets.__eq__(self, packet) and \
           self.packetIdentifier == packet.packetIdentifier and \
           self.topicFilters == packet.topicFilters


class Unsubacks(UnsubSubacks):

  def __init__(self, buffer=None, DUP=False, QoS=0, RETAIN=False, PacketId=1, reasonCodes=[]):
      UnsubSubacks.__init__(self, PacketTypes.UNSUBACK, buffer, DUP, QoS, RETAIN,
          PacketId, reasonCodes)


class Pingreqs(Packets):

  def __init__(self, buffer=None, DUP=False, QoS=0, RETAIN=False):
    object.__setattr__(self, "names", ["fh", "DUP", "QoS", "RETAIN"])
    self.fh = FixedHeaders(PacketTypes.PINGREQ)
    self.fh.DUP = DUP
    self.fh.QoS = QoS
    self.fh.RETAIN = RETAIN
    if buffer != None:
      self.unpack(buffer)

  def unpack(self, buffer, maximumPacketSize):
    assert len(buffer) >= 2
    assert PacketType(buffer) == PacketTypes.PINGREQ
    fhlen = self.fh.unpack(buffer, maximumPacketSize)
    assert self.fh.remainingLength == 0
    assert self.fh.DUP == False, "[MQTT-2.1.2-1]"
    assert self.fh.QoS == 0, "[MQTT-2.1.2-1]"
    assert self.fh.RETAIN == False, "[MQTT-2.1.2-1]"
    return fhlen

  def __str__(self):
    return str(self.fh)+")"


class Pingresps(Packets):

  def __init__(self, buffer=None, DUP=False, QoS=0, RETAIN=False):
    object.__setattr__(self, "names", ["fh", "DUP", "QoS", "RETAIN"])
    self.fh = FixedHeaders(PacketTypes.PINGRESP)
    self.fh.DUP = DUP
    self.fh.QoS = QoS
    self.fh.RETAIN = RETAIN
    if buffer != None:
      self.unpack(buffer)

  def unpack(self, buffer, maximumPacketSize):
    assert len(buffer) >= 2
    assert PacketType(buffer) == PacketTypes.PINGRESP
    fhlen = self.fh.unpack(buffer, maximumPacketSize)
    assert self.fh.remainingLength == 0
    assert self.fh.DUP == False, "[MQTT-2.1.2-1]"
    assert self.fh.QoS == 0, "[MQTT-2.1.2-1]"
    assert self.fh.RETAIN == False, "[MQTT-2.1.2-1]"
    return fhlen

  def __str__(self):
    return str(self.fh)+")"


class Disconnects(Packets):

  def __init__(self, buffer=None, DUP=False, QoS=0, RETAIN=False,
          reasonCode="Normal disconnection"):
    object.__setattr__(self, "names",
        ["fh", "DUP", "QoS", "RETAIN", "reasonCode", "properties"])
    self.fh = FixedHeaders(PacketTypes.DISCONNECT)
    self.fh.DUP = DUP
    self.fh.QoS = QoS
    self.fh.RETAIN = RETAIN
    # variable header
    self.reasonCode = ReasonCodes(PacketTypes.DISCONNECT, aName=reasonCode)
    self.properties = Properties(PacketTypes.DISCONNECT)
    if buffer != None:
      self.unpack(buffer)

  def pack(self):
    buffer = b""
    if self.reasonCode.getName() != "Normal disconnection" or not self.properties.isEmpty():
      buffer += self.reasonCode.pack()
      if not self.properties.isEmpty():
        buffer += self.properties.pack()
    buffer = self.fh.pack(len(buffer)) + buffer
    return buffer

  def unpack(self, buffer, maximumPacketSize):
    self.properties.clear()
    self.reasonCode.set("Normal disconnection")
    assert len(buffer) >= 2
    assert PacketType(buffer) == PacketTypes.DISCONNECT
    fhlen = self.fh.unpack(buffer, maximumPacketSize)
    assert len(buffer) >= fhlen + self.fh.remainingLength
    assert self.fh.DUP == False, "[MQTT-2.1.2-1] DISCONNECT reserved bits must be 0"
    assert self.fh.QoS == 0, "[MQTT-2.1.2-1] DISCONNECT reserved bits must be 0"
    assert self.fh.RETAIN == False, "[MQTT-2.1.2-1] DISCONNECT reserved bits must be 0"
    curlen = fhlen
    if self.fh.remainingLength > 0:
      self.reasonCode.unpack(buffer[curlen:])
      curlen += 1
    if self.fh.remainingLength > 1:
      curlen += self.properties.unpack(buffer[curlen:])[1]
    assert curlen == fhlen + self.fh.remainingLength, \
            "DISCONNECT packet is wrong length %d" % self.fh.remainingLength
    return fhlen + self.fh.remainingLength

  def __str__(self):
    return str(self.fh)+", ReasonCode: "+str(self.reasonCode)+", Properties: "+str(self.properties)

  def __eq__(self, packet):
    return Packets.__eq__(self, packet) and \
           self.reasonCode == packet.reasonCode and \
           self.properties == packet.properties


class Auths(Packets):

  def __init__(self, buffer=None, DUP=False, QoS=0, RETAIN=False,
          reasonCode="Success"):
    object.__setattr__(self, "names",
        ["fh", "DUP", "QoS", "RETAIN", "reasonCode", "properties"])
    self.fh = FixedHeaders(PacketTypes.AUTH)
    self.fh.DUP = DUP
    self.fh.QoS = QoS
    self.fh.RETAIN = RETAIN
    # variable header
    self.reasonCode = ReasonCodes(PacketTypes.AUTH, reasonCode)
    self.properties = Properties(PacketTypes.AUTH)
    if buffer != None:
      self.unpack(buffer)

  def pack(self):
    buffer = self.reasonCode.pack()
    buffer += self.properties.pack()
    buffer = self.fh.pack(len(buffer)) + buffer
    return buffer

  def unpack(self, buffer, maximumPacketSize):
    assert len(buffer) >= 2
    assert PacketType(buffer) == PacketTypes.AUTH
    fhlen = self.fh.unpack(buffer, maximumPacketSize)
    assert len(buffer) >= fhlen + self.fh.remainingLength
    assert self.fh.DUP == False, "[MQTT-2.1.2-1] AUTH reserved bits must be 0"
    assert self.fh.QoS == 0, "[MQTT-2.1.2-1] AUTH reserved bits must be 0"
    assert self.fh.RETAIN == False, "[MQTT-2.1.2-1] AUTH reserved bits must be 0"
    curlen = fhlen
    curlen += self.reasonCode.unpack(buffer[curlen:])
    curlen += self.properties.unpack(buffer[curlen:])[1]
    assert curlen == fhlen + self.fh.remainingLength, \
            "AUTH packet is wrong length %d %d" % (self.fh.remainingLength, curlen)
    return fhlen + self.fh.remainingLength

  def __str__(self):
    return str(self.fh)+", ReasonCode: "+str(self.reasonCode)+", Properties: "+str(self.properties)

  def __eq__(self, packet):
    return Packets.__eq__(self, packet) and \
           self.reasonCode == packet.reasonCode and \
           self.properties == packet.properties


classes = [Connects, Connacks, Publishes, Pubacks, Pubrecs,
           Pubrels, Pubcomps, Subscribes, Subacks, Unsubscribes,
           Unsubacks, Pingreqs, Pingresps, Disconnects, Auths]

def unpackPacket(buffer, maximumPacketSize=MAX_PACKET_SIZE):
  if PacketType(buffer) != None:
    packet = classes[PacketType(buffer)-1]()
    packet.unpack(buffer, maximumPacketSize=maximumPacketSize)
  else:
    packet = None
  return packet
