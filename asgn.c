
/**
* File: asgn2.c
* Date: 22/09/2017
* Author: Patrick Skinner
* Version: 0.1
*
* Note: multiple devices and concurrent modules are not supported in this
*       version.
*/

/* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version
* 2 of the License, or (at your option) any later version.
*/

#include "gpio.h"
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/device.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>

#define MYDEV_NAME "asgn2"
#define MYIOC_TYPE 'k'

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Patrick Skinner");
MODULE_DESCRIPTION("COSC440 asgn2");

/**
* The node structure for the memory page linked list.
*/
typedef struct page_node_rec {
	struct list_head list;
	struct page *page;
	int eof_position;
} page_node;

typedef struct asgn2_dev_t {
	dev_t dev;            /* the device */
	struct cdev *cdev;
	struct list_head mem_list;
	int num_pages;        /* number of memory pages this module currently holds */
	size_t data_size;     /* total data size in this module */
	atomic_t nprocs;      /* number of processes accessing this device */ 
	atomic_t max_nprocs;  /* max number of processes accessing this device */
	struct kmem_cache *cache;      /* cache memory */
	struct class *class;     /* the udev class */
	struct device *device;   /* the udev device node */
} asgn2_dev;

struct asgn2_buffer {
	char *buf;
	int read_index;
	int write_index;
	int size;
} buffer;

int read_pos;
int write_pos;

asgn2_dev asgn2_device;
struct prod_dir_entry *asgn2_proc;

int asgn2_major = 0;                      /* major number of module */  
int asgn2_minor = 0;                      /* minor number of module */
int asgn2_dev_count = 1;                  /* number of devices */

int firstHalfByte = 0;
int secondHalfByte = 0;
bool isFirstHalf = true;

void write_tasklet(unsigned long t_arg);
static DECLARE_TASKLET(t_name, write_tasklet, (unsigned long) &buffer);

spinlock_t my_lock;
DECLARE_WAIT_QUEUE_HEAD(dataQueue);
atomic_t data_ready;
DECLARE_WAIT_QUEUE_HEAD(deviceQueue);
atomic_t device_ready;

bool read_eof;
int files;				/* number of files in page list */

/**
* This function frees all memory pages held by the module.
*/

void free_memory_pages(void) {
	page_node *curr; /* pointer to list head object */
	page_node *temp; /* temporary list head for safe deletion */


	/*Loop through the entire page list*/
	list_for_each_entry_safe(curr, temp, &asgn2_device.mem_list, list){

		/* If node has a page, free the page. */
		if(curr->page != NULL){
			__free_page(curr->page);
		}

		/* Remove node from page list, free the node. */
		list_del(&curr->list);
		kfree(curr);
	}

	/* reset device data size, and num_pages */
	asgn2_device.num_pages = 0;
	asgn2_device.data_size = 0;
}


/**
* This function opens the virtual disk, if it is opened in the write-only
* mode, all memory pages will be freed.
*/
int asgn2_open(struct inode *inode, struct file *filp) {

	if(atomic_read(&asgn2_device.nprocs) >= atomic_read(&asgn2_device.max_nprocs)){
		printk(KERN_WARNING "New Reader Placed on Wait Queue.");
		atomic_set(&device_ready, 0);
		wait_event_interruptible_exclusive(deviceQueue, atomic_read(&device_ready));
	} else {
		atomic_inc(&asgn2_device.nprocs);
	}

	/* If opened in write-only mode, free all memory pages */
	//if(filp->f_mode == FMODE_WRITE){
	if( (filp->f_flags & O_ACCMODE) != O_RDONLY){
		printk(KERN_WARNING "Device Can Only Be Opened In Read Only Mode");
		//return -EACCES;
	}

	read_eof = 0;
	printk(KERN_INFO "Device Succesfully Opened\n");
	return 0; /* Success */
}


/**
* This function releases the virtual disk, but nothing needs to be done in this case.
*/
int asgn2_release (struct inode *inode, struct file *filp) {

	/* Decrement process count */
	atomic_dec(&asgn2_device.nprocs);

	/* Set device ready flag, wake up a process from deviceQueue */
	atomic_set(&device_ready, 1);
	wake_up_interruptible(&deviceQueue);

	printk(KERN_INFO "Successfuly Released, Current NPROCS: %d\n", atomic_read(&asgn2_device.nprocs));
	return 0;
}

/* Write content from buf into our multiple page queue, allocating new pages as required. */
ssize_t write(const char *buf, size_t count, bool eof_flag){
	size_t size_written = 0;  /* size written to virtual disk in this function */
        size_t begin_offset;      /* the offset from the beginning of a page to start writing. */

	int begin_page_no = write_pos / PAGE_SIZE; /* first page to write to */

	int curr_page_no = 0;
	size_t curr_size_written;
	size_t size_to_be_written;

	int nPages;

	page_node *curr;

	nPages = (write_pos + count + (PAGE_SIZE-1) )/PAGE_SIZE;

	printk(KERN_WARNING "BYTE TO WRITE: %c", *buf);

	while(asgn2_device.num_pages < nPages){
		printk(KERN_INFO "Adding Page");
                curr = kmalloc(sizeof(page_node), GFP_KERNEL);

                if(curr){
                        curr->page = alloc_page(GFP_KERNEL);
                        if(curr->page == NULL){
                                printk(KERN_INFO "Memory Allocation Failed");
                                return -ENOMEM;
                        }
                } else {
                        printk(KERN_INFO "Memory Allocation Failed");
                        return -ENOMEM;
                }

		curr->eof_position = -1;

                list_add_tail( &(curr->list), &asgn2_device.mem_list);
                asgn2_device.num_pages++;

		printk(KERN_INFO "Memory Allocation Successful");
	}

        printk(KERN_INFO "nPages = %d, devPages = %d, count = %d", nPages, asgn2_device.num_pages, count);

	list_for_each_entry(curr, &asgn2_device.mem_list, list){
		if(curr_page_no >= begin_page_no && size_written < count){
                        begin_offset = write_pos % PAGE_SIZE;

                        size_to_be_written = min( (int) PAGE_SIZE - begin_offset, count-size_written);
			//size_to_be_written = 1;

                        //printk(KERN_INFO "Page Found");
                        printk(KERN_INFO "Written: %d, Count: %d, To Write: %d", size_written, count, size_to_be_written);

                        //while(size_written < count){}
                        curr_size_written = 0;

			//while( curr_size_written < size_to_be_written ){
                                memcpy(page_address(curr->page)+begin_offset, buf+size_written, size_to_be_written);
				curr_size_written = size_to_be_written;
				size_written += curr_size_written;
                                begin_offset += curr_size_written;
                                write_pos += curr_size_written;

                                printk(KERN_INFO "Size Written: %d, Remaining: %d\n", size_written, count-size_written);
                        //}
                //}

	        if(eof_flag == true){
	                printk(KERN_WARNING "Current EOF = %d", curr->eof_position);
        	        if(curr->eof_position < 0){
                	        //curr->eof_position = begin_offset -1;
				printk(KERN_WARNING "Wrote EOF at: %d, Page: %d", (int)(write_pos%PAGE_SIZE)-1, curr_page_no);
				curr->eof_position = (write_pos % PAGE_SIZE)-1;
				//files++;
				//printk(KERN_WARNING "Files++");
				//break;
			}

			files++;
                        printk(KERN_WARNING "Files++");

        	}
		}

		curr_page_no++;

	}


	//asgn2_device.data_size = max(asgn2_device.data_size, size_written);
	//asgn2_device.data_size += size_written;
	asgn2_device.data_size = write_pos - read_pos;

	atomic_set(&data_ready, 1);
	wake_up_interruptible(&dataQueue);

	return size_written;

}

/* Write bytrs to our circular buffer and schedule tasklets to write them to our page queue, if 
	our buffer is full any incoming bytes are rejected until space is made available. */
void writeToBuffer(char byte){
        int nextIndex = (buffer.write_index + 1) % buffer.size;

        // Check if buffer is full
        if( nextIndex == buffer.read_index ){
                printk(KERN_WARNING "Buffer Full, Rejecting Incoming Byte");
                return;
        } else {
                buffer.buf[buffer.write_index] = byte;
                buffer.write_index = nextIndex;

		//schedule tasklet
		tasklet_schedule(&t_name);
		return;
        }
}

/* Assemble half byte pairs into whole byte which are then sent to our circular buffer. */
irqreturn_t dummyport_interrupt(int irq, void *dev_id){
	char byte;

	printk(KERN_WARNING "Interrupt Received");
	if(isFirstHalf){
		firstHalfByte = read_half_byte();
		isFirstHalf = false;
	} else {
		secondHalfByte = read_half_byte();
		isFirstHalf = true;

		byte = (char) firstHalfByte << 4 | secondHalfByte;
		printk(KERN_WARNING "Byte formed: %c", byte);

		//write byte to circular buffer
		writeToBuffer(byte);
	}
	return 0;
}

/* Take bytes from our circular buffer and write them into our page queue. this function
	primarily handles the state of our circular buffer. */
void write_tasklet(unsigned long t_arg){
	int written;
	int notWritten;
	bool eof_flag;
	eof_flag = false;

	// Check if there is data to read from the buffer
	if( buffer.read_index == buffer.write_index){
		printk(KERN_WARNING "Nothing to be read from buffer");
		return;
	}

	// Check for end of file
	if(buffer.buf[buffer.read_index] == '\0'){
		printk(KERN_WARNING "TASKLET EOF");
		eof_flag = true;
	}

	//Check if write index is below read index, if so we've wrapped around buffer
	if(buffer.write_index < buffer.read_index){
		printk(KERN_WARNING "Tasklet Branch One");

		spin_lock( &my_lock);
                written = write( &buffer.buf[buffer.read_index], buffer.write_index + buffer.size - buffer.read_index , eof_flag);

                notWritten = (buffer.write_index + buffer.size - buffer.read_index) - written;

		printk(KERN_WARNING "Written: %d, Write Index: %d, Read Index: %d, Not Written: %d, Byte: %c", written, buffer.write_index, buffer.read_index, notWritten, buffer.buf[buffer.read_index]);

                buffer.read_index += written;
		buffer.read_index = (buffer.read_index % buffer.size);

                spin_unlock( &my_lock);
	} else {
		printk(KERN_WARNING "Tasklet Branch Two");

		spin_lock( &my_lock);
		//int written = write( &buffer.buf[buffer.read_index], buffer.write_index - buffer.read_index, 0 );
		written = write( &buffer.buf[buffer.read_index], buffer.write_index - buffer.read_index , eof_flag);

		notWritten = (buffer.write_index - buffer.read_index) - written;

		printk(KERN_WARNING "Written: %d, Write Index: %d, Read Index: %d, Not Written: %d, Byte: %c", written, buffer.write_index, buffer.read_index, notWritten, buffer.buf[buffer.read_index]);

		buffer.read_index += written;
		spin_unlock( &my_lock);
	}

	return;
}

/**
* This function reads contents of the virtual disk and writes to the user 
*/
ssize_t asgn2_read(struct file *filp, char __user *buf, size_t count,
loff_t *f_pos) {
	size_t size_read = 0;     /* size read from virtual disk in this function */
	size_t begin_offset;      /* the offset from the beginning of a page to start reading */
	int begin_page_no = read_pos / PAGE_SIZE; /* the first page which contains the requested data */
	int curr_page_no = 0;     /* the current page number */
	size_t curr_size_read;    /* size read from the virtual disk in this round */
	size_t size_to_be_read = 0;   /* size to be read in the current round in while loop */

	int bytes;
	size_t tempSize;

	//struct list_head *ptr = asgn2_device.mem_list.next;
	page_node *curr;
	page_node *temp;

	/**
	* Traverse the list, once the first requested page is reached,
	*   - use copy_to_user to copy the data to the user-space buf page by page
	*   - you also need to work out the start / end offset within a page
	*   - Also needs to handle the situation where copy_to_user copy less
	*       data than requested, and
	*       copy_to_user should be called again to copy the rest of the
	*       unprocessed data, and the second and subsequent calls still
	*       need to check whether copy_to_user copies all data requested.
	*       This is best done by a while / do-while loop.
	*

	* if end of data area of ramdisk reached before copying the requested
	*   return the size copied to the user space so far
	*/

	printk(KERN_WARNING "Entering Read Function");

	/* check f_pos, if beyond data_size, return 0. */
	if( asgn2_device.data_size <= 0 ) {
		printk(KERN_WARNING "data_size <= 0");
	}

	if(read_eof){
		printk(KERN_INFO "EOF Read, Returning. Data Size: %d", asgn2_device.data_size);
		return 0;
	}

	if(read_pos == write_pos) {
		// No data to read but no EOF reached, go to sleep.
		atomic_set(&data_ready, 0);
		wait_event_interruptible(dataQueue, atomic_read(&data_ready));
	}

	/* Traverse the list. */
	list_for_each_entry_safe(curr, temp, &asgn2_device.mem_list, list){
		printk(KERN_WARNING "Curr: %d, Begin: %d, Data Size: %zu", curr_page_no, begin_page_no, asgn2_device.data_size);

		/* Reached requested page. */
		if(curr_page_no >= begin_page_no && size_read < count){
			begin_offset = read_pos % PAGE_SIZE;

			if(count >= asgn2_device.data_size){ count = asgn2_device.data_size; }

			printk(KERN_WARNING "EOF_Position: %d", curr->eof_position);
			if(curr->eof_position >= 0){
				printk(KERN_WARNING "EOF on current page");
				read_eof = true;

				size_to_be_read = (curr->eof_position + 1) - read_pos;
				printk(KERN_WARNING "Size To Be Read: %d, Read Pos: %d", size_to_be_read, read_pos);

				if(files > 1){
					printk(KERN_INFO "Files > 1");
					bytes = curr->eof_position + 1;
					tempSize = asgn2_device.data_size;
					curr->eof_position = -1;
					asgn2_device.data_size = tempSize;

					while(bytes < PAGE_SIZE){
						char *byte;
						byte = (page_address(curr->page) + bytes);
						if( *byte == '\0'){
							printk(KERN_WARNING "Second EOF on page, pos: %d", bytes);
							tempSize = asgn2_device.data_size;
							curr->eof_position = bytes;
							asgn2_device.data_size = tempSize;
							break;
						}
						bytes++;
					}
				}

			} else {
				printk(KERN_WARNING "You Got Up In Here Uce");
				if(count >= asgn2_device.data_size){ count = asgn2_device.data_size; }
	                        size_to_be_read = min( (int) PAGE_SIZE - begin_offset, (int) count - size_read);
			}

			printk(KERN_INFO "Read: %d, Count: %d, To Read: %d", size_read, count, size_to_be_read);

			curr_size_read = 0;

			/* use copy_to_user to copy the data to the user-space buf */
			while(curr_size_read < size_to_be_read){
				curr_size_read = size_to_be_read - copy_to_user(buf + size_read,
				page_address(curr->page) + begin_offset,
				size_to_be_read);

				size_read += curr_size_read;
				size_to_be_read -= curr_size_read;
				begin_offset += curr_size_read;
				read_pos += curr_size_read;
				*f_pos += curr_size_read;
				//printk(KERN_INFO "Curr Size Read: %d, Size to be Read: %d", curr_size_read, size_to_be_read);
				printk(KERN_INFO "Size Read: %d, Remaining: %d", size_read, count - size_read);
				if(size_read == 0) { break; }
				break;
			}

			if( read_pos > asgn2_device.data_size){
				printk(KERN_INFO "READ_POS greater than data_size");
				//break;
			};

		}
		
		//printk(KERN_WARNING "!DATA SIZE = %zu", asgn2_device.data_size);
		curr_page_no++;
		//printk(KERN_WARNING "File Count: %d, Read EOF: %d", files, (int) read_eof);
		
		/* End of file reached */
		if(read_eof){
			if( (size_read%PAGE_SIZE) > asgn2_device.data_size){
				asgn2_device.data_size = 0;
			} else {
                        	asgn2_device.data_size -= (size_read % PAGE_SIZE);
			}
			files--;
                        printk(KERN_WARNING "File Count: %d", files);

                        if(files > 0){ // eof count
                                return size_read;
                        } else {
                                if(asgn2_device.data_size == 0){
                                        printk(KERN_INFO "Resetting");
                                        //printk(KERN_WARNING "Post Data Size = %zu, fpos: %lld", asgn2_device.data_size, f_pos);
                                        write_pos = 0;
                                        read_pos = 0;
                                        //f_pos = 0;
                                        curr->eof_position = -1;
                                        asgn2_device.data_size = 0;
                                        files = 0;
                                        //printk(KERN_WARNING "Post Data Size = %zu, fpos: %lld", asgn2_device.data_size, f_pos);
                                } else {
                                        printk(KERN_INFO "Data to read but no EOF");
                                        //printk(KERN_WARNING "Data Size = %zu", asgn2_device.data_size);
                                        tempSize = asgn2_device.data_size;
                                        curr->eof_position = -1;
                                        asgn2_device.data_size = tempSize;
                                        read_eof = false;
                                        //printk(KERN_WARNING "Post Data Size = %zu", asgn2_device.data_size);

                                }
                        }
                }



		/* free unneeded pages */
		else if(read_pos == PAGE_SIZE || asgn2_device.data_size > PAGE_SIZE){
			printk(KERN_WARNING "Freeing Page");
			if(curr->page != NULL) __free_page(curr->page);
			list_del(&curr->list);
			kfree(curr);
			write_pos -= PAGE_SIZE;
			read_pos = 0;
			//asgn2_device.data_size -= PAGE_SIZE;
			printk(KERN_WARNING "ds: %zu", asgn2_device.data_size);
			/*if(size_read != PAGE_SIZE){
				asgn2_device.data_size -= (size_read % PAGE_SIZE);
			} else {
				asgn2_device.data_size -= size_read;
			}*/
			if( PAGE_SIZE > asgn2_device.data_size){
                                asgn2_device.data_size = 0;
                        } else {
                                asgn2_device.data_size -= PAGE_SIZE;
                        }

			printk(KERN_WARNING "ds2: %zu", asgn2_device.data_size);
			asgn2_device.num_pages--;
		}
		
		//curr_page_no++;
	}
	
	printk(KERN_WARNING "Read %d bytes. \n\n", (int)size_read);
	return size_read;
}

#define SET_NPROC_OP 1
#define TEM_SET_NPROC _IOW(MYIOC_TYPE, SET_NPROC_OP, int) 

/**
* The ioctl function, which nothing needs to be done in this case.
*/
long asgn2_ioctl (struct file *filp, unsigned cmd, unsigned long arg) {
	int nr;
	int new_nprocs;
	int result;

	printk(KERN_INFO "Entering IOCTL Function");

	/* check whether cmd is for our device, if not for us, return -EINVAL */
	if(MYIOC_TYPE !=  _IOC_TYPE(cmd)){
		printk(KERN_WARNING "Command not for asgn2_device, returning");
		return -EINVAL;
	}

	/* get command, if command is SET_NPROC_OP, then get the data, and set max_nprocs accordingly. */
	nr = _IOC_NR(cmd);
	if( nr == SET_NPROC_OP){
		/*check validity of the value before setting max_nprocs*/
		if( access_ok(VERIFY_READ, arg, sizeof(cmd))){

			result = __get_user(new_nprocs, (int*) arg);
			if( result != 0 ){ /* Bad Access from User Space. */
				printk(KERN_WARNING "Bad Access from User Space\n");
				return -EFAULT;
			}

			if( new_nprocs != 1 ){ /* Max nprocs must be one. */
				printk(KERN_WARNING "New max nprocs less/greater than one");
				return -EINVAL;
			}

			atomic_set(&asgn2_device.max_nprocs, new_nprocs); /* Update max_nprocs. */
			printk(KERN_INFO "Max nprocs update successful");
			return 0; /* Success. */

		} else {
			printk(KERN_WARNING "access_ok() failed, unallowed access");
			return -EFAULT; /* Access not allowed. */
		}

	}

	printk(KERN_WARNING "Bad command for driver");
	return -ENOTTY; /* Command not applicable to this driver */

}


struct file_operations asgn2_fops = {
	.owner = THIS_MODULE,
	.read = asgn2_read,
	.unlocked_ioctl = asgn2_ioctl,
	.open = asgn2_open,
	.release = asgn2_release,
};


static void *my_seq_start(struct seq_file *s, loff_t *pos)
{
	if(*pos >= 1) return NULL;
	else return &asgn2_dev_count + *pos;
}

static void *my_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	(*pos)++;
	if(*pos >= 1) return NULL;
	else return &asgn2_dev_count + *pos;
}

static void my_seq_stop(struct seq_file *s, void *v)
{
	/* There's nothing to do here! */
}

int my_seq_show(struct seq_file *s, void *v) {
/**
* use seq_printf to print some info to s
*/
	seq_printf(s,"Major Number: %d\n Minor Number: %d\n Num Pages: %d\n Data Size: %zu\n Nprocs: %d\n Max-Nprocs: %d\n",
	asgn2_major, asgn2_minor, asgn2_device.num_pages, asgn2_device.data_size,
	atomic_read(&asgn2_device.nprocs), atomic_read(&asgn2_device.max_nprocs));

	seq_printf(s, "Files: %d\nRead Pos: %d\nWrite Pos: %d\n\n", files, read_pos, write_pos);

	seq_printf(s, "\n");

	return 0;


}


static struct seq_operations my_seq_ops = {
	.start = my_seq_start,
	.next = my_seq_next,
	.stop = my_seq_stop,
	.show = my_seq_show
};

static int my_proc_open(struct inode *inode, struct file *filp)
{
	return seq_open(filp, &my_seq_ops);
}

struct file_operations asgn2_proc_ops = {
	.owner = THIS_MODULE,
	.open = my_proc_open,
	.read = seq_read,
	.release = seq_release,
};



/**
* Initialise the module and create the master device
*/
int __init asgn2_init_module(void){
	int result;

	/* set nprocs and max_nprocs of the device */
	atomic_set(&asgn2_device.nprocs, 0);
	atomic_set(&asgn2_device.max_nprocs, 1);

	/* Allocate Major/Minor number */
	asgn2_device.dev = MKDEV(asgn2_major, asgn2_minor);
	result = alloc_chrdev_region(&asgn2_device.dev, asgn2_minor, asgn2_dev_count, MYDEV_NAME);
	if(result != 0){
		printk(KERN_WARNING "Major/Minor number allocation failed");
		goto fail_device;
	}

	/*Allocate cdev and set ops and owner field */
	asgn2_device.cdev = cdev_alloc();
	cdev_init(asgn2_device.cdev, &asgn2_fops);
	asgn2_device.cdev->owner = THIS_MODULE; 
	result = cdev_add(asgn2_device.cdev, asgn2_device.dev, asgn2_dev_count);
	if(result != 0){
		printk(KERN_WARNING "CDEV Initialisation Failed");
		goto fail_device;
	}

	/* Initialise page list */
	INIT_LIST_HEAD(&asgn2_device.mem_list);

	/* Create proc entries */
	asgn2_proc = proc_create(MYDEV_NAME, 0, NULL, &asgn2_proc_ops);
	if(asgn2_proc == NULL) goto fail_a;

	asgn2_device.class = class_create(THIS_MODULE, MYDEV_NAME);
	if (IS_ERR(asgn2_device.class)) {
		goto fail_b;
	}

	asgn2_device.device = device_create(asgn2_device.class, NULL, 
	asgn2_device.dev, "%s", MYDEV_NAME);
	if (IS_ERR(asgn2_device.device)) {
		printk(KERN_WARNING "%s: can't create udev device\n", MYDEV_NAME);
		result = -ENOMEM;
		goto fail_c;
	}

	printk(KERN_WARNING "set up udev entry\n");

	gpio_dummy_init();

	buffer.size = PAGE_SIZE;
	buffer.buf = kmalloc(sizeof(char) * buffer.size, GFP_KERNEL);
	buffer.read_index = 0;
	buffer.write_index = 0;

	printk(KERN_WARNING "Circular Buffer Initialised");

	spin_lock_init(&my_lock);

	printk(KERN_WARNING "Hello world from %s\n", MYDEV_NAME);
	return 0;

	/* cleanup code called when any of the initialization steps fail */
	fail_a:
	remove_proc_entry(MYDEV_NAME, NULL);

	fail_b:
	cdev_del(asgn2_device.cdev);

	fail_c:
	unregister_chrdev_region(asgn2_device.dev, asgn2_dev_count);

	fail_device:
	class_destroy(asgn2_device.class);
	kfree(buffer.buf);
	return result;
}


/**
* Finalise the module
*/
void __exit asgn2_exit_module(void){
	device_destroy(asgn2_device.class, asgn2_device.dev);
	class_destroy(asgn2_device.class);
	printk(KERN_WARNING "cleaned up udev entry\n");

	gpio_dummy_exit();

	/**
	* free all pages in the page list
	* cleanup in reverse order
	*/
	free_memory_pages();

	kfree(buffer.buf);

	remove_proc_entry(MYDEV_NAME, NULL);
	cdev_del(asgn2_device.cdev);
	unregister_chrdev_region(asgn2_device.dev, asgn2_dev_count);
	printk(KERN_WARNING "Good bye from %s\n", MYDEV_NAME);
}


module_init(asgn2_init_module);
module_exit(asgn2_exit_module);


