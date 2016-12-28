/*
 * device_attacher.c
 *
 *  Created on: 2016. okt. 13.
 *      Author: Tusori Tibor
 */


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
#include <linux/interrupt.h>
#include <linux/string.h>

#include "linux/of_fdt.h"
#include "linux/firmware.h"

static unsigned int id_interrupt = 0;
static struct resource id_reg_res;
static void  __iomem *id_reg_base_addr;


static struct workqueue_struct *wq;

/*
#define __init
#define __exit
*/

static int overlay_id = -1;
static struct device_node *new_node = NULL;
static void *dev_tree_blob;
// BOTTOM HALF WORKER
void load_overlay(struct work_struct* ws)
{
	struct firmware *fw;
	int ret;
	unsigned long id;
	char f_name[21];

	// Delete previous overlay
	if(overlay_id >=0 ) of_overlay_destroy(overlay_id);
	if(new_node) {of_node_put(new_node); new_node = NULL;}
	if(dev_tree_blob) {kfree(dev_tree_blob); dev_tree_blob = NULL;}
	printk(KERN_DEBUG"Previous overlay deleted\n");
	// Read device id
	id= ioread32(id_reg_base_addr);

	// Create file name from device id
	snprintf(f_name,21,"dev_%lu.dtbo",id);

	// Request firmware
	request_firmware((const struct firmware **)&fw,f_name,NULL);

	if(!fw)
	{
		printk(KERN_ERR"Device tree overlay not found.\n");
		goto err;
	}
	printk(KERN_DEBUG"Firmware found\n");

	// copy blob
	dev_tree_blob = kmalloc(fw->size,GFP_KERNEL);
	if(!dev_tree_blob)
	{
		printk(KERN_ERR"No memory for firmware.\n");
		release_firmware(fw);
		goto err;
	}
	memcpy(dev_tree_blob,fw->data,fw->size);
	release_firmware(fw);

	of_fdt_unflatten_tree((unsigned long*)dev_tree_blob,&new_node);

	if(!new_node)
	{
		printk(KERN_ERR"Cannot unflatten device tree blob.\n");
		goto err0;
	}
	of_node_set_flag(new_node,OF_DETACHED);

	// Resolve phandles in the new device tree fragment
	ret = of_resolve_phandles(new_node);
	if(ret!=0)
	{
		printk(KERN_ERR"Cannot resolve phandles in the device tree fragment.\n");
		goto err1;
	}

	printk(KERN_DEBUG"Inserting the new overlay.\n");

	// Inserting device tree overlay
	overlay_id = of_overlay_create(new_node);
	if(overlay_id < 0)
	{
		printk(KERN_ERR"Cannot add device tree overlay.\n");
		goto err1;
	}

	switch(id)
	{
	case 1: printk(KERN_INFO"AXI pwm device detected.\n"); break;
			break;
	case 2: printk(KERN_INFO"AXI random device detected.\n"); break;
			break;
	case 3: printk(KERN_INFO"AXI switch device detected.\n"); break;
			break;
	case 4: printk(KERN_INFO"AXI timer device detected.\n"); break;
			break;
	default: break;
	}

	return;

err1:
	of_node_put(new_node);
	new_node = NULL;
err0:
	kfree(dev_tree_blob);
	dev_tree_blob = NULL;
err:
	overlay_id = -1;
	return;
}
DECLARE_WORK(load_job,load_overlay);

// TOP HALF INTERRUPT HANDLER
irqreturn_t da_int_handler(int irq,void *devid)
{
	// Unset irq flag
	iowrite32(0,id_reg_base_addr);
	// Starting workqueue, that loads the matching overlay.
	queue_work(wq,&load_job);
	return IRQ_HANDLED;
}



static int id_reg_overlay_id;
static struct device_node *overlay_node;
static uint8_t* id_blob = NULL;

// Module parameters
static short startup_check = 0;
module_param(startup_check,short,0644);
MODULE_PARM_DESC(startup_check,"In case of not 0 value the device ID is read after the succesfull module loading.");

static int  da_init(void)
{
	int retval;
	struct firmware *id_fw;
	struct device_node *id_node;

	printk(KERN_INFO"Device attacher module started.\n");

	printk(KERN_DEBUG"Loading device tree overlay of the id register.\n");
	request_firmware_direct(&id_fw,"axi_id_reg.dtbo",NULL);
	if(!id_fw)
	{
		printk(KERN_ERR"Overlay for id reg not found.\n");
		goto err;
	}
	// Copy device tree blob
	id_blob = kmalloc(id_fw->size,GFP_KERNEL);
	if(!id_blob)
	{
		printk(KERN_ERR"No memory for device tree blob.\n");
		release_firmware(id_fw);
		goto err;
	}
	memcpy(id_blob,id_fw->data,id_fw->size);
	release_firmware(id_fw);

	of_fdt_unflatten_tree((unsigned long*)id_blob,&overlay_node);
	if(!overlay_node)
	{
		printk(KERN_ERR"Cannot unflatten id reg overlay.\n");
		goto err0;
	}
	of_node_set_flag(overlay_node,OF_DETACHED);
	retval = of_resolve_phandles(overlay_node);
	if(retval!=0)
	{
		printk(KERN_ERR"Cannot resolve phandles in the id reg overlay.\n");
		goto err1;
	}
	id_reg_overlay_id = of_overlay_create(overlay_node);
	if(id_reg_overlay_id < 0)
	{
		printk(KERN_ERR"Cannot add id reg overlay to the device tree.\n");
		goto err1;
	}

	printk(KERN_DEBUG"Accessing id reg parameters.\n");
	// Find the id register in the device tree
	id_node = of_find_compatible_node(NULL,NULL,"xlnx,my-id-reg-2.0");
	if(!id_node)
	{
		printk(KERN_ERR"Cant find id reg node.\n");
		goto err2;
	}

	// Read the interrupt number and remap the physical address range to the kernel address space.
	// interrupt line
	id_interrupt = irq_of_parse_and_map(id_node,0);
	if(id_interrupt == 0)
	{
		printk(KERN_ERR"Couldn't get the used irq number.\n");
		goto err3;
	}
	printk(KERN_DEBUG"Used linux irq number: %d.\n",id_interrupt);

	// Base address
	// Get resource
	retval = of_address_to_resource(id_node,0,&id_reg_res);
	if(retval)
	{
		printk(KERN_ERR"Cannot get the address resource from the device tree.\n");
		goto err3;
	}
	// Allocate it
	if(!request_mem_region(id_reg_res.start,resource_size(&id_reg_res),"Peripheral Identifier"))
	{
		printk(KERN_ERR"Cannot allocate memory region.\n");
		goto err3;
	}
	// Remap to kernel space
	id_reg_base_addr = ioremap(id_reg_res.start,4); //We allocate just one word,it is enough, it is useless to remap the full 64kB region.
	if(!id_reg_base_addr)
	{
		printk(KERN_ERR"Cannot map id reg to kernel space.\n");
		goto err4;
	}

	// Initializing workqueue.
	wq = create_workqueue("Device Attacher workqueue");
	if(!wq)
	{
		printk(KERN_ERR"Cannot create workqueue.\n");
		goto err5;
	}

	// Register handler to the interrupt line.
	if(request_irq(id_interrupt, da_int_handler,0,"Device Attacher",NULL))
	{
		printk(KERN_ERR"Interrupt line occupied.\n");
		goto err6;
	}

	printk(KERN_INFO"Device Attacher loaded successfully.\n");
	of_node_put(id_node);

	// If startup_check module parameter is not 0, perform the ID check.
	if(startup_check != 0)
		queue_work(wq,&load_job);
	return 0;


	err6:
		destroy_workqueue(wq);
	err5:
		iounmap(id_reg_base_addr);
	err4:
		release_mem_region(id_reg_res.start,resource_size(&id_reg_res));
	err3:
		of_node_put(id_node);
	err2:
		of_overlay_destroy(id_reg_overlay_id);
	err1:
		of_node_put(overlay_node);
	err0:
		if(id_blob) kfree(id_blob);
	err:
		return -EFAULT;
}

static void __exit  da_exit(void)
{
	// Delete current device tree overlay
	if(overlay_id >= 0) of_overlay_destroy(overlay_id);
	if(new_node) {of_node_put(new_node);}
	if(dev_tree_blob) kfree(dev_tree_blob);

	free_irq(id_interrupt,NULL);
	destroy_workqueue(wq);
	iounmap(id_reg_base_addr);
	release_mem_region(id_reg_res.start,resource_size(&id_reg_res));
	of_overlay_destroy(id_reg_overlay_id);
	of_node_put(overlay_node);
	if(id_blob) kfree(id_blob);
	printk(KERN_INFO"Device Attacher unloaded.\n");
}

module_init(da_init);
module_exit(da_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tusori Tibor");
MODULE_DESCRIPTION("Reads identifier from the PL peripeheral, and loads the appropriate device tree overlay.");
