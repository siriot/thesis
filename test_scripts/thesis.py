import os.path
import time
from math import sin,pi

while True:
    sw_state = ""
    
    # load swich peripheral
	# if the switch driver is already loaded, no need to do it again
    if not os.path.exists("/dev/sw"):
		os.system("cat /sd/bit/my_axi_sw.bit > /dev/xdevcfg")
		# wait for the device file to appear
		while not os.path.exists("/dev/sw"):
			pass
	
    # get switch state
    with open("/dev/sw","r") as sw_file:
        sw_state = sw_file.readline()
        
    # load axi_led periperal
    os.system("cat /sd/bit/my_axi_pwm.bit > /dev/xdevcfg")
	# wait for all the device files
    while not os.path.exists("/dev/led_pwm0"):
        pass
    while not os.path.exists("/dev/led_pwm7"):
        pass
    
    # device is loaded, start showing the state
    max_brightness = 100000
    for t in range(11):
        for led_num in range(8):
            if sw_state[led_num] == '1':
                with open("/dev/led_pwm"+str(7-led_num),'w') as dev_file:
                    dev_file.write( str(int(max_brightness * sin(2*pi/20*t))) )
        time.sleep(0.3)
