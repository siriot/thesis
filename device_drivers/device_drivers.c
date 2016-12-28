/*
 * device_drivers.c
 *
 *	Drivers for the example devices.
 *
 *  Created on: 2016. okt. 13.
 *      Author: Tusori Tibor
 */
/********************************************************* INCLUDES ********************************************************************/
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/errno.h>
#include <linux/workqueue.h>
#include <asm-generic/irq.h>
#include <asm-generic/errno.h>
#include <asm-generic/errno-base.h>
#include <asm-generic/io.h>
#include <linux/interrupt.h>
#include <linux/of_device.h>
#include <linux/irqdomain.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>

#include <linux/string.h>

u32 str2int(const char*str,int len);
int uint2str(u32 num, char*str,size_t len);

/*
#define __init
#define __exit*/

/******************************************************
 * ***** Global driver functions and structures *******
 * ****************************************************/

// Own data stucture, containing the data of the character device(s).
struct chardev_data_type{
	struct cdev char_dev;
	struct class *myclass;
	struct device *interface_device[8];
	int Major;
	int minor_num;
	void __iomem *base;
};

// Own data structure, containing the data of the platform device.
struct device_data{
	int irq_num;
	struct resource res;
	struct chardev_data_type chardev_data;
	void __iomem *base;
};

/**
 * create_chardev - creates character devices with the given parameters
 * @chardev_data: Output structure
 * @name_base: Name for the character device. If more devices are required, an index number is appended to the name.
 * @num: Number of the required character devices.
 * @fops: Structure containing functions that implement the file operations.
 */
int create_chardev(struct chardev_data_type *chardev_data,const char* name_base, int num, struct file_operations *fops)
{
	// Locals
	int retval;
	int i;
	dev_t dev_num;

	if(num < 1 || num > 8)
	{
		printk(KERN_ERR"Invalid minor number.\n");
		return -EINVAL;
	}
	chardev_data->minor_num = num;

	if(alloc_chrdev_region(&dev_num,0,num,name_base)<0)
	{
		retval = -ENODEV;
		printk(KERN_ERR"Chardev number allocation failed.\n");
		goto err1;
	}
	chardev_data->Major = MAJOR(dev_num);
	printk(KERN_DEBUG"Major number allocation succeeded: %d.\n",chardev_data->Major);

	// Init cdev
	cdev_init(&chardev_data->char_dev,fops);
	chardev_data->char_dev.count = 0;
	chardev_data->char_dev.owner = THIS_MODULE;
	chardev_data->char_dev.dev = dev_num;
	retval = cdev_add(&chardev_data->char_dev,dev_num,num);
	if(retval < 0)
	{
		retval = -ENODEV;
		goto err2;
	}

	// Create class
	printk(KERN_DEBUG"Creating class for udev.\n");
	chardev_data->myclass = class_create(THIS_MODULE,name_base);
	printk(KERN_DEBUG"Creating character devices.\n");
	// Create device
	if(num == 1)
	{
		chardev_data->interface_device[0] = device_create(chardev_data->myclass,NULL,MKDEV(chardev_data->Major,0),NULL,name_base);
	}
	else
	{
		char name[20];
		int len;

		len = strnlen(name_base,18);
		strncpy(name,name_base,18);
		name[19] = 0;

		for(i=0;i<num;i++)
		{
			name[len] = '0' + i;
			chardev_data->interface_device[i] = device_create(chardev_data->myclass,NULL,MKDEV(chardev_data->Major,i),NULL,name);
		}
	}

	printk(KERN_DEBUG"Chardev creation succeeded.\n");
	return 0;

	err2:
	unregister_chrdev_region(dev_num,num);
	err1:
	return retval;
}

/**
 * Removes the character devices from the kernel.
 * @chardev_data: Pointer to the chardev_data_type filled by create_chardev.
 */
int remove_chardev(struct chardev_data_type *chardev_data)
{
	// Locals
	int i;

	if(!chardev_data) return -ENODATA;

	printk(KERN_DEBUG"Removing character device.\n");
	for(i=0;i<chardev_data->minor_num;i++)
		device_destroy(chardev_data->myclass,MKDEV(chardev_data->Major,i));
	class_destroy(chardev_data->myclass);

	//Unregister character device
	cdev_del(&chardev_data->char_dev);
	unregister_chrdev_region(MKDEV(chardev_data->Major,0),chardev_data->minor_num);
	return 0;
}

/// Length of the remapped address space for the devices.
#define IOREMAP_SIZE 64

/**
 * alloc_resources - Allocates the interrupt line and memory region used by the device, and saves the informations about them as driver_data in the platform_device.
 * @pdev: Platform device to be used.
 * @ name_base, num, fops: Parameters for create_chardev.
 */
static int alloc_resources(struct platform_device *pdev,const char* name_base, int num, struct file_operations *fops)
{
	//Locals
	int retval;
	struct device_data *data;

	printk(KERN_DEBUG"Allocating platform device resources.\n");

	// Allocating container struct for device data.
	data = kmalloc(sizeof(struct device_data),GFP_KERNEL);
	if(!data)
	{
		printk(KERN_ERR"Insufficient memory.\n");
		return -ENOMEM;
	}
	memset(data,0,sizeof(struct device_data));

	// Getting memory resource
	retval = of_address_to_resource(pdev->dev.of_node,0,&data->res);
	if(retval != 0)
	{
		printk(KERN_ERR"Cannot get address resource of the dplatform device.\n");
		retval = -ENODATA;
		goto err1;
	}

	if(!request_mem_region(data->res.start,resource_size(&data->res),pdev->name))
	{
		printk(KERN_ERR"Cannot allocate memory region from: %x.\n",data->res.start);
		retval = -EBUSY;
		goto err1;
	}

	printk(KERN_DEBUG"Physical address start: 0x%x.\n",data->res.start);

	//Remapping hw address to kernel space.
	data->base = ioremap(data->res.start,IOREMAP_SIZE);
	data->chardev_data.base = data->base;
	if(!data->base)
	{
		retval = -EIO;
		goto err2;
	}

	printk(KERN_DEBUG"Remapped base address: 0x%x.\n",(unsigned int)data->base);

	// Remap device interrupt, if exists
	data->irq_num = irq_of_parse_and_map(pdev->dev.of_node,0);
	if(data->irq_num > 0) printk(KERN_DEBUG"Used interrupt line: %d.\n",data->irq_num);

	// Save data to platform_device
	platform_set_drvdata(pdev,data);

	//Create character device
	retval = create_chardev(&(data->chardev_data),name_base,num,fops);
	if(retval)
	{
		printk(KERN_ERR"Character device creation failed.\n");
		goto err3;
	}


	return 0;
	err3:
		iounmap(data->base);
	err2:
		release_mem_region(data->res.start,resource_size(&data->res));
	err1:
		kfree(data);
		platform_set_drvdata(pdev,NULL);
	return retval;
}

/**
 * free_resources - Deallocates the resources stored in the driver_data field of the platform device.
 * @pdev: Pointer to the actual platform_device.
 */
static int free_resources(struct platform_device *pdev)
{
	// Locals
	struct device_data *data;

	data = platform_get_drvdata(pdev);
	if(!data) goto err;
	remove_chardev(&(data->chardev_data));
	iounmap(data->base);
	data->base = NULL;
	data->chardev_data.base = NULL;
	release_mem_region(data->res.start,resource_size(&data->res));
	kfree(data);
	platform_set_drvdata(pdev,NULL);
	printk(KERN_DEBUG"Device resources are deallocated.\n");

err:
	return -ENODATA;
}


static int general_open(struct inode * inode, struct file *pfile)
{
	try_module_get(THIS_MODULE);
	struct chardev_data_type *data;

	if(!inode->i_cdev) return -ENODEV;
	// Store a pointer to the chardev_data in the file structure, so that the read/write functions can use the base address in it.
	data = container_of(inode->i_cdev,struct chardev_data_type,char_dev);
	if(!data) return -ENODEV;
	pfile->private_data = data;

	return 0;
}


static int general_close(struct inode * inode, struct file *pfile)
{
	module_put(THIS_MODULE);
	return 0;
}
/******************************************************************************
 * 							SWITCH DRIVER
 ******************************************************************************/

	///////////////////////// SWITCH FILE OPERATIONS ////////////////////////////////


	/**
	 * sw_read - Returns the state of the switches as a string containing 1-s and 0-s.
	 */
	static ssize_t sw_read (struct file *pfile, char __user *buff, size_t count, loff_t *ppos)
	{
		u32 val;
		char bin[9];
		int i;

		// Get the base address from the file structure.
		void __iomem *ks_sw_base = ((struct chardev_data_type*)pfile->private_data)->base;
		if(!ks_sw_base) return -ENODEV;

		val = ioread32(ks_sw_base);
		// Convert the LSB to binary
		for(i=0;i<8;i++)
		{
			bin[7-i] = '0'+val%2;
			val /= 2;
		}
		bin[8] = 0;

		if(*ppos > 8) return 0;
		if(*ppos + count > 9) count = 9 - *ppos;
		if(copy_to_user(buff,bin + *ppos,count)) return -EFAULT;
		*ppos += count;
		return count;
	}

	static ssize_t sw_write (struct file *pfile, const char __user *buff, size_t count, loff_t *ppos)
	{

		printk(KERN_ERR"Switch device is not writable.\n");
		return 0;
	}

	static struct file_operations sw_fops =
	{
			.owner = THIS_MODULE,
			.open = general_open,
			.release = general_close,
			.read = sw_read,
			.write = sw_write
	};

///////////////////////// SW PLATFORM DRIVER FUNCTIONS ////////////////////////

static int sw_probe(struct platform_device *pdev)
{
	// Locals
	int retval;

	printk(KERN_DEBUG"Probing switch driver.\n");
	retval = alloc_resources(pdev,"sw",1,&sw_fops);
	if(retval) return retval;

	printk(KERN_INFO"Switch driver loaded.\n");
	return 0;
}

static int sw_remove(struct platform_device *pdev)
{
	return free_resources(pdev);
}

static struct of_device_id sw_match_table[]={
		{.compatible = "xlnx,my-axi-sw-1.0"},{}
};

static struct platform_driver sw_driver={
		.driver={
				.name = "sw",
				.owner = THIS_MODULE,
				.of_match_table = sw_match_table
		},
		.probe = sw_probe,
		.remove = sw_remove
};



/*******************************************************************************
 * 						Random number generator
 *******************************************************************************/

	///////////////////////// File operations /////////////////////////////////////

	static ssize_t rng_read (struct file *pfile, char __user *buff, size_t count, loff_t *ppos)
	{
		u32 val;
		char str[11];
		int len;
		void __iomem *ks_rng_base = ((struct chardev_data_type*)(pfile->private_data))->base;
		if(!ks_rng_base) return -ENODEV;

		// Return data only if there is enough place for them. No numbers are skipped this way.
		if (count<2) return 0;

		val = ioread16(ks_rng_base);

		if( copy_to_user(buff, (uint8_t*)&val,2)) return -EFAULT;
		else return 2;
	}

	static ssize_t rng_write (struct file *pfile, const char __user *buff, size_t count, loff_t *ppos)
	{

		u32 val = 0;
		void __iomem *ks_rng_base = ((struct chardev_data_type*)(pfile->private_data))->base;
		if(!ks_rng_base) return -ENODEV;

		if(count==0) return 0;
		copy_from_user((void*)&val,buff,count>2?2:count);

		printk(KERN_DEBUG"New random number generator seed: %d.\n",val);

		iowrite32(val,ks_rng_base);
		return count;
	}

	static struct file_operations rng_fops =
	{
			.owner = THIS_MODULE,
			.open = general_open,
			.release = general_close,
			.read = rng_read,
			.write = rng_write
	};

/////////////////////// Platform driver functions /////////////////////////////

	static int rng_probe(struct platform_device *pdev)
	{
		int retval;

		printk(KERN_DEBUG"Probing random number generator driver.\n");
		retval = alloc_resources(pdev,"myrandom",1,&rng_fops);
		if(retval) return retval;

		printk(KERN_INFO"Random number driver loaded.\n");
		return 0;
	}


	static int rng_remove(struct platform_device *pdev)
	{
		return free_resources(pdev);
	}


	static struct of_device_id rng_match_table[]={
			{.compatible = "xlnx,my-axi-random-1.0"},{}
	};

	static struct platform_driver rng_driver={
			.driver={
					.name = "random",
					.owner = THIS_MODULE,
					.of_match_table = rng_match_table
			},
			.probe = rng_probe,
			.remove = rng_remove
	};



/*******************************************************************************
 * 						AXI TIMER DRIVER
 *******************************************************************************/


	///////////////////////// File operations ///////////////////////////////////

	/*
	 * timer_read - Gives the interrupt period as ASCII string.
	 */
	static ssize_t timer_read (struct file *pfile, char __user *buff, size_t count, loff_t *ppos)
	{
		u32 period;
		char period_str[11];
		int str_len;
		void __iomem *ks_timer_base = ((struct chardev_data_type*)pfile->private_data)->base;
		if(!ks_timer_base) return -ENODEV;

		period = ioread32(ks_timer_base + 4);
		str_len = uint2str(period,period_str,11);

		if(*ppos >= str_len) return 0;
		if(*ppos +count > str_len) count = str_len-*ppos;
		if(copy_to_user(buff,period_str+*ppos,count)) return -EFAULT;
		*ppos += count;

		return count;
	}

	/**
	 * Resets the timer with the given value. If value is less then 100000, timer is stopped. (Frquency is 100Mhz).
	 */
	static ssize_t timer_write (struct file *pfile, const char __user *buff, size_t count, loff_t *ppos)
	{
		u32 val;
		char ks_str[11];
		int ks_len;
		void __iomem *ks_timer_base = ((struct chardev_data_type*)pfile->private_data)->base;
		if(!ks_timer_base) return -ENODEV;

		ks_len = count>10?10:count;
		ks_str[ks_len] = 0;
		if(copy_from_user(ks_str,buff,ks_len)) {return -EFAULT;}


		// Convert input to u32.
		val = str2int(ks_str,ks_len);

		printk(KERN_DEBUG"Value converted: %d.\n",val);
		if(val < 100000)
		{
			//Stop timer
			iowrite32(0x172,ks_timer_base);
			printk(KERN_DEBUG"AXI timer is stopped, because period %d is too small.(It should be greater or equal than 100000.)\n",val);
		}
		else
		{
			// Set reset value ad reset the timer.
			iowrite32(val,ks_timer_base + 4);	// Set reset value
			iowrite32(0x172,ks_timer_base);		// Reset timer
			iowrite32(0x1D2,ks_timer_base);		// Start timer
			printk(KERN_DEBUG"AXI Timer is reseted with period: %d.\n",val);
		}
		return count;
	}


	static struct file_operations timer_fops=
	{
			.owner = THIS_MODULE,
			.open = general_open,
			.release = general_close,
			.read = timer_read,
			.write = timer_write
	};

	////////////////////////////// interrupt handler /////////////////////////////
	static int irq_num;
	static struct workqueue_struct *w_queue = NULL;
	void timer_irq_worker(struct work_struct*);
	DECLARE_WORK(task,timer_irq_worker);
	void __iomem *id_address;

	// Interrupt handler - TOP HALF
	irqreturn_t timer_irq_handler(int irq, void *dev_id)
	{
		// Clearing interrupt flag.
		u32 reg_val;
		reg_val = ioread32(id_address);
		iowrite32(reg_val,id_address);
		queue_work(w_queue,&task);
		return IRQ_HANDLED;
	}


	// Interrupt handler - BOTTOM HALF
	void timer_irq_worker(struct work_struct* ws)
	{
		printk(KERN_DEBUG"AXI TIMER interrupt occurred.\n");
	}
/////////////////////////////// Platform driver functions ////////////////////////

	static int timer_probe(struct platform_device *pdev)
	{
		// Locals
		int retval;
		struct device_data *data;

		printk(KERN_DEBUG"Probing axi_timer driver.\n");
		retval = alloc_resources(pdev,"mytimer",1,&timer_fops);
		if(retval) return retval;

		data = (struct device_data*)platform_get_drvdata(pdev);
		id_address = data->base;

		// Registering interrupt handler
		if(request_irq(data->irq_num,timer_irq_handler,0,"AXI_TIMER",NULL))
		{
			printk(KERN_ERR"The interrupt %d is already taken.\n",irq_num);
			retval = -EBUSY;
			goto err0;
		}

		// Creating workqueue.
		w_queue = create_workqueue("AXI_TIMER_workqueue");
		if(!w_queue)
		{
			printk(KERN_ERR"Cannot create workqueue.\n");
			retval = -ENOMEM;
			goto err1;
		}

		printk(KERN_INFO"AXI timer driver loaded.\n");
		return 0;

		err1:
			free_irq(data->irq_num,NULL);
		err0:
			free_resources(pdev);
		return retval;
	}

	static int timer_remove(struct platform_device *pdev)
	{
		struct device_data *data = (struct device_data*)platform_get_drvdata(pdev);
		if(!data) return 0;

		destroy_workqueue(w_queue);
		free_irq(data->irq_num,NULL);
		return free_resources(pdev);
	}

	static struct of_device_id timer_match_table[]={
			{.compatible =  "xlnx,xps-timer-1.00.a"},{}
	};


	static struct platform_driver timer_driver={
			.driver={
					.name = "timer",
					.owner = THIS_MODULE,
					.of_match_table = timer_match_table
			},
			.probe = timer_probe,
			.remove = timer_remove
	};
/*******************************************************************************
 * 						LED_PWM DRIVER
 *******************************************************************************/


///////////////// LED PWM File operations module //////////////////////////////

		/**
		 * led_pwm_read - Returns the brightness of the leds as an ASCII string.
		 */
		static ssize_t led_pwm_read (struct file *pfile, char __user *buff, size_t count, loff_t *ppos)
		{
			//Locals
			int minor;
			u32 pwm_val;
			int len = 0;
			char str[11];
			void __iomem *ks_led_pwm_base = ((struct chardev_data_type*)pfile->private_data)->base;
			if(!ks_led_pwm_base) return -ENODEV;

			// Getting the minor number, it tells, which led should be modified.
			minor = MINOR(pfile->f_inode->i_rdev);
			pwm_val = ioread32(ks_led_pwm_base+minor*4);

			len = uint2str(pwm_val,str,11);

			if(*ppos >= len) return 0;
			if(*ppos+count > len) count = len - *ppos;
			if(copy_to_user(buff,str+*ppos,count)) return -EFAULT;
			*ppos+=count;
			return count;
		}

		/**
		 * led_pwm_write - Sets the brightness of the leds.
		 */
		static ssize_t led_pwm_write (struct file *pfile, const char __user *buff, size_t count, loff_t *ppos)
		{
			int minor;

			char str[11];
			int len;
			u32 val = 0;
			void __iomem *ks_led_pwm_base = ((struct chardev_data_type*)pfile->private_data)->base;
			if(!ks_led_pwm_base) return -ENODEV;

			// Getting minor number
			minor = MINOR(pfile->f_inode->i_rdev);

			len = count>10?10:count;
			if(copy_from_user(str,buff,len)) return -EFAULT;
			str[len] = 0;

			val = str2int(str,len);

			// write the value to the register
			iowrite32(val,ks_led_pwm_base + minor*4);

			return count;
		}

		static struct file_operations led_pwm_fops=
		{
				.owner = THIS_MODULE,
				.read = led_pwm_read,
				.write = led_pwm_write,
				.open = general_open,
				.release = general_close
		};

///////////////////////////////// LED_PWM Platform driver functions  /////////////////////////////////////////////

static int led_pwm_probe(struct platform_device *pdev)
{
	// Locals
	int retval;

	printk(KERN_DEBUG"Probing led_pwm driver.\n");
	retval = alloc_resources(pdev,"led_pwm",8,&led_pwm_fops);
	if(retval) return retval;


	printk(KERN_INFO"PWM led driver loaded.\n");
	return 0;
}

int led_pwm_remove(struct platform_device *pdev)
{
	return free_resources(pdev);
}

static struct of_device_id led_pwm_match_table[]={
		{.compatible = "xlnx,my-axi-pwm-1.0"},{}
};

static struct platform_driver led_pwm_driver={
		.driver={
				.name = "led_pwm",
				.owner = THIS_MODULE,
				.of_match_table = led_pwm_match_table
		},
		.probe = led_pwm_probe,
		.remove = led_pwm_remove
};
/******************************************************** Misc functions ******************************************************************/
u32 str2int(const char*str,int len)
{
	int i;
	u32 val;

	for(val=0, i=0; str[i] >= '0' && str[i] <= '9' && i<len; i++)
	{
		val *=10;
		val += str[i]-'0';
	}
	return val;
}
int uint2str(u32 num, char*str,size_t len)
{
	int l;
	char rev[15];
	int i;
	size_t digit_num;

	if(num == 0)
	{
		rev[0] = '0';
		l = 1;
	}
	else
	for(l=0;num>0;l++)
	{
		rev[l] = '0' + num %10;
		num /= 10;
	}

	if(len-1 > l)
		digit_num = l;
	else
		digit_num = len-1;


	for(i=0;i<digit_num;i++)
	{
		str[digit_num-1-i] = rev[i];
	}
	str[digit_num] = 0;

	return digit_num+1;
}
/***************************************************** Misc functions END ******************************************************************/

/* MODULE FUNCTIONS */

/**
 * device_drivers_init - Registers all the platform drivers implemented in this file.
 */
static int __init device_drivers_init(void)
{
	printk(KERN_INFO"Loading PL peripheral drivers.\n");

	platform_driver_register(&led_pwm_driver);
	printk(KERN_DEBUG"PWM led driver registered.\n");

	platform_driver_register(&sw_driver);
	printk(KERN_DEBUG"Switch driver registered.\n");

	platform_driver_register(&rng_driver);
	printk(KERN_DEBUG"Random number generator driver registered.\n");

	platform_driver_register(&timer_driver);
	printk(KERN_DEBUG"AXI timer driver registered.\n");

	return 0;
}

/**
 * device_drivers_exit - Unregisters the platform drivers.
 */
static void __exit device_drivers_exit(void)
{
	printk(KERN_INFO"Unloading PL peripheral drivers.\n");
	platform_driver_unregister(&led_pwm_driver);
	platform_driver_unregister(&sw_driver);
	platform_driver_unregister(&rng_driver);
	platform_driver_unregister(&timer_driver);
}

module_init(device_drivers_init);
module_exit(device_drivers_exit);

MODULE_DESCRIPTION("Drivers for PL periperals.\n");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tusori Tibor");

