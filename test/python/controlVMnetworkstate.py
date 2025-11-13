#!/usr/bin/python
import os
import sys
import time
import subprocess
import random
bindir='/usr/bin'
sys.path.append(bindir)

def input_sel(prompt,max_,selectionoption):
    # let the user choose the VM and verify selection
    while True:
        try:
            print ('Please select from the list of running VMs\n\n'+'\n'.join(selectionoption))
            userin = int(raw_input(prompt))
        except ValueError:
            print('\nThat was not a number\n\n')
            continue
        if userin > max_:
            print('\nInput must be less than or equal to {0}.\n\n'.format(max_))
        elif userin < 1:
            print('\nInput must be greater than or equal to 1\n\n')
        else:
            return userin

def statustext(result):
    if result == 0:
        status = 'OK'
    else:
        status = 'Failed'
    return status

def controlvmnetworkstate():
    try:
        offtime = 600
        ontime = 14
        vmdict={}
        vmlist=[]
        executable = os.path.join(bindir, 'VBoxManage')

        #retrieve a list of all running VMs
        runningvms= subprocess.check_output('%s list runningvms' %executable,shell=True).splitlines()
        if len(runningvms) != 0:
            for n in range(0, len(runningvms)):
                vmlist.append('%s: %s' %(n+1,runningvms[n].rsplit(' ',1)[0].strip('"')))
                vmdict[n+1]=runningvms[n].rsplit(' ',1)[-1]
            usersel=input_sel('\nEnter the number of the VM: ',len(runningvms),vmlist)

        else:
            print('Can not retrieve list of running VMs')
            sys.exit()

        vmuuid=vmdict[usersel]
        while True:
            offtime = random.randint(60, 90)
            ontime = random.randint(10, 90)
            timenow = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime())
            on = subprocess.call('%s controlvm %s setlinkstate1 on' %(executable,vmuuid),
                                 shell=True)
            status=statustext(on)
            print ('%s: Plug Network cable into VM %s for %ds: %s' % (timenow, runningvms[usersel-1].rsplit(' ',1)[0].strip('"'),ontime, str(status)))
            time.sleep(ontime)
            timenow = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime())
            off = subprocess.call('%s controlvm %s setlinkstate1 off' %(executable,vmuuid),
                                  shell=True)
            status = statustext(off)
            print ('%s: Unplug Network cable from VM %s for %ds: %s' % (timenow, runningvms[usersel-1].rsplit(' ',1)[0].strip('"'),offtime, str(status)))
            time.sleep(offtime)
    except KeyboardInterrupt:
        sys.exit('\nUser Interrupt')
    except Exception as e:
        print("Error in %s in function %s: %s" % (__name__, sys._getframe().f_code.co_name, e.message))
if __name__ == "__main__":
    sys.exit(controlvmnetworkstate())
