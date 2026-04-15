/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include "aesd_ioctl.h"
#include "aesdchar.h"
#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/fs.h> // file_operations
#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/types.h>

int aesd_major = 0; // use dynamic major
int aesd_minor = 0;
struct aesd_dev aesd_device;

MODULE_AUTHOR("Your Name Here"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

int aesd_open(struct inode *inode, struct file *filp) {
  struct aesd_dev *dev;
  PDEBUG("open");
  /**
   * TODO: handle open
   */
  dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
  filp->private_data = dev;
  return 0;
}

int aesd_release(struct inode *inode, struct file *filp) {
  PDEBUG("release");
  /**
   * TODO: handle release
   */
  return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                  loff_t *f_pos) {
  /**
   * TODO: handle read
   */
  ssize_t retval = 0;
  struct aesd_dev *dev = filp->private_data;
  struct aesd_buffer_entry *entry;
  size_t entry_offset;
  size_t bytes_to_copy;
  PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

  if (mutex_lock_interruptible(&dev->lock)) {
    return -ERESTARTSYS;
  }
  entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *f_pos,
                                                          &entry_offset);

  if (entry == NULL) {
    retval = 0;
    goto cleanup;
  }

  bytes_to_copy = min(count, entry->size - entry_offset);

  if (copy_to_user(buf, entry->buffptr + entry_offset, bytes_to_copy)) {
    retval = -EFAULT;
    goto cleanup;
  }

  *f_pos += bytes_to_copy;
  retval = bytes_to_copy;

cleanup:
  mutex_unlock(&dev->lock);
  return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                   loff_t *f_pos) {
  ssize_t retval = -ENOMEM;
  struct aesd_dev *dev = filp->private_data;
  char *new_buf;
  const char *newline_pos;

  PDEBUG("write %zu bytes with offset %lld", count, *f_pos);

  if (mutex_lock_interruptible(&dev->lock)) {
    return -ERESTARTSYS;
  }

  new_buf = krealloc(dev->entry_in_progress.buffptr,
                     dev->entry_in_progress.size + count, GFP_KERNEL);

  if (new_buf == NULL) {
    mutex_unlock(&dev->lock);
    return -ENOMEM;
  }

  if (copy_from_user(new_buf + dev->entry_in_progress.size, buf, count)) {
    mutex_unlock(&dev->lock);
    return -EFAULT;
  }

  dev->entry_in_progress.buffptr = new_buf;
  dev->entry_in_progress.size += count;

  newline_pos =
      memchr(dev->entry_in_progress.buffptr, '\n', dev->entry_in_progress.size);

  if (newline_pos != NULL) {
    if (dev->buffer.full) {
      kfree(dev->buffer.entry[dev->buffer.out_offs].buffptr);
      dev->buffer.entry[dev->buffer.out_offs].buffptr = NULL;
    }

    aesd_circular_buffer_add_entry(&dev->buffer, &dev->entry_in_progress);

    dev->entry_in_progress.buffptr = NULL;
    dev->entry_in_progress.size = 0;
  }

  retval = count;

  mutex_unlock(&dev->lock);
  return retval;
}

static size_t aesd_get_total_size(struct aesd_dev *dev) {
  size_t total = 0;
  uint8_t index;
  struct aesd_buffer_entry *entry;

  AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->buffer, index) {
    total += entry->size;
  }
  return total;
}

loff_t aesd_llseek(struct file *filp, loff_t offset, int whence) {
  loff_t retval;
  struct aesd_dev *dev = filp->private_data;
  size_t total_size;

  PDEBUG("llseek offset %lld whence %d", offset, whence);

  if (mutex_lock_interruptible(&dev->lock)) {
    return -ERESTARTSYS;
  }

  total_size = aesd_get_total_size(dev);
  retval = fixed_size_llseek(filp, offset, whence, total_size);

  mutex_unlock(&dev->lock);
  return retval;
}

long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
  struct aesd_dev *dev = filp->private_data;
  struct aesd_seekto seekto;
  long retval = 0;
  size_t offset = 0;
  uint8_t i;
  uint8_t actual_index;
  uint8_t actual_cmd_index;

  PDEBUG("ioctl cmd %u arg %lu", cmd, arg);

  switch (cmd) {
  case AESDCHAR_IOCSEEKTO:
    if (copy_from_user(&seekto, (struct aesd_seekto __user *)arg,
                       sizeof(seekto))) {
      return -EFAULT;
    }

    if (mutex_lock_interruptible(&dev->lock)) {
      return -ERESTARTSYS;
    }

    if (seekto.write_cmd >= AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED) {
      retval = -EINVAL;
      goto cleanup;
    }

    actual_cmd_index = (dev->buffer.out_offs + seekto.write_cmd) %
                       AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

    if (dev->buffer.entry[actual_cmd_index].buffptr == NULL) {
      retval = -EINVAL;
      goto cleanup;
    }

    if (seekto.write_cmd_offset >= dev->buffer.entry[actual_cmd_index].size) {
      retval = -EINVAL;
      goto cleanup;
    }

    for (i = 0; i < seekto.write_cmd; i++) {
      actual_index =
          (dev->buffer.out_offs + i) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
      offset += dev->buffer.entry[actual_index].size;
    }

    offset += seekto.write_cmd_offset;
    filp->f_pos = offset;

  cleanup:
    mutex_unlock(&dev->lock);
    break;

  default:
    retval = -ENOTTY;
    break;
  }

  return retval;
}

struct file_operations aesd_fops = {
    .owner = THIS_MODULE,
    .llseek = aesd_llseek,
    .read = aesd_read,
    .write = aesd_write,
    .open = aesd_open,
    .release = aesd_release,
    .unlocked_ioctl = aesd_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev) {
  int err, devno = MKDEV(aesd_major, aesd_minor);

  cdev_init(&dev->cdev, &aesd_fops);
  dev->cdev.owner = THIS_MODULE;
  dev->cdev.ops = &aesd_fops;
  err = cdev_add(&dev->cdev, devno, 1);
  if (err) {
    printk(KERN_ERR "Error %d adding aesd cdev", err);
  }
  return err;
}

int aesd_init_module(void) {
  dev_t dev = 0;
  int result;
  result = alloc_chrdev_region(&dev, aesd_minor, 1, "aesdchar");
  aesd_major = MAJOR(dev);
  if (result < 0) {
    printk(KERN_WARNING "Can't get major %d\n", aesd_major);
    return result;
  }
  memset(&aesd_device, 0, sizeof(struct aesd_dev));

  mutex_init(&aesd_device.lock);
  aesd_circular_buffer_init(&aesd_device.buffer);
  /**
   * TODO: initialize the AESD specific portion of the device
   */

  result = aesd_setup_cdev(&aesd_device);

  if (result) {
    unregister_chrdev_region(dev, 1);
  }
  return result;
}

void aesd_cleanup_module(void) {
  uint8_t index;
  struct aesd_buffer_entry *entry;

  dev_t devno = MKDEV(aesd_major, aesd_minor);
  cdev_del(&aesd_device.cdev);

  /**
   * TODO: cleanup AESD specific poritions here as necessary
   */

  if (aesd_device.entry_in_progress.buffptr != NULL) {
    kfree(aesd_device.entry_in_progress.buffptr);
  }

  AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
    if (entry->buffptr != NULL) {
      kfree(entry->buffptr);
    }
  }
  unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
