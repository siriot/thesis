import os
import time
import struct

# load random number generator peripheral if neccessary
if not os.path.exists("/dev/myrandom"):
	os.system("cat /sd/bit/my_axi_rng.bit > /dev/xdevcfg")
	# wait for the device file to appear
	while not os.path.exists("/dev/myrandom"):
	    pass

# init the random number generator
with open("/dev/myrandom","w") as f:
    init_data = bytearray([1,1])
    f.write(init_data)
    
print "Random number generator inicialized."

with open("/dev/myrandom","r") as f:
	while True:
		word = bytearray(f.read(2))
		longval = struct.unpack("H",word)[0]
      		bin_form = ""
		# convert to binary
		for i in range(16):
			if longval%2 == 0:
				bin_form = bin_form + "0"
			else:
				bin_form = bin_form + "1"
			longval = longval/2
        	print bin_form
    		time.sleep(0.5)
