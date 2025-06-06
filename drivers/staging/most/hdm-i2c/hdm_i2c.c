/*
 * hdm_i2c.c - Hardware Dependent Module for I2C Interface
 *
 * Copyright (C) 2013-2015, Microchip Technology Germany II GmbH & Co. KG
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * This file is licensed under GPLv2.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/err.h>

#include <mostcore.h>

enum { CH_RX, CH_TX, NUM_CHANNELS };

#define MAX_BUFFERS_CONTROL 32
#define MAX_BUF_SIZE_CONTROL 256

/**
 * list_first_mbo - get the first mbo from a list
 * @ptr:	the list head to take the mbo from.
 */
#define list_first_mbo(ptr) \
	list_first_entry(ptr, struct mbo, list)

/* IRQ / Polling option */
static bool polling_req;
module_param(polling_req, bool, S_IRUGO);
MODULE_PARM_DESC(polling_req, "Request Polling. Default = 0 (use irq)");

/* Polling Rate */
static int scan_rate = 100;
module_param(scan_rate, int, 0644);
MODULE_PARM_DESC(scan_rate, "Polling rate in times/sec. Default = 100");

struct hdm_i2c {
	bool is_open[NUM_CHANNELS];
	bool polling_mode;
	struct most_interface most_iface;
	struct most_channel_capability capabilities[NUM_CHANNELS];
	struct i2c_client *client;
	struct rx {
		struct delayed_work dwork;
		wait_queue_head_t waitq;
		struct list_head list;
		struct mutex list_mutex;
	} rx;
	char name[64];
};

#define to_hdm(iface) container_of(iface, struct hdm_i2c, most_iface)

/**
 * configure_channel - called from MOST core to configure a channel
 * @iface: interface the channel belongs to
 * @channel: channel to be configured
 * @channel_config: structure that holds the configuration information
 *
 * Return 0 on success, negative on failure.
 *
 * Receives configuration information from MOST core and initialize the
 * corresponding channel.
 */
static int configure_channel(struct most_interface *most_iface,
			     int ch_idx,
			     struct most_channel_config *channel_config)
{
	struct hdm_i2c *dev = to_hdm(most_iface);

	BUG_ON(ch_idx < 0 || ch_idx >= NUM_CHANNELS);
	BUG_ON(dev->is_open[ch_idx]);

	if (channel_config->data_type != MOST_CH_CONTROL) {
		pr_err("bad data type for channel %d\n", ch_idx);
		return -EPERM;
	}

	if (channel_config->direction != dev->capabilities[ch_idx].direction) {
		pr_err("bad direction for channel %d\n", ch_idx);
		return -EPERM;
	}

	if ((channel_config->direction == MOST_CH_RX) && (dev->polling_mode)) {
		queue_delayed_work(system_power_efficient_wq, &dev->rx.dwork,
				      msecs_to_jiffies(MSEC_PER_SEC / 4));
	}
	dev->is_open[ch_idx] = true;

	return 0;
}

/**
 * enqueue - called from MOST core to enqueue a buffer for data transfer
 * @iface: intended interface
 * @channel: ID of the channel the buffer is intended for
 * @mbo: pointer to the buffer object
 *
 * Return 0 on success, negative on failure.
 *
 * Transmit the data over I2C if it is a "write" request or push the buffer into
 * list if it is an "read" request
 */
static int enqueue(struct most_interface *most_iface,
		   int ch_idx, struct mbo *mbo)
{
	struct hdm_i2c *dev = to_hdm(most_iface);
	int ret;

	BUG_ON(ch_idx < 0 || ch_idx >= NUM_CHANNELS);
	BUG_ON(!dev->is_open[ch_idx]);

	if (ch_idx == CH_RX) {
		/* RX */
		mutex_lock(&dev->rx.list_mutex);
		list_add_tail(&mbo->list, &dev->rx.list);
		mutex_unlock(&dev->rx.list_mutex);
		wake_up_interruptible(&dev->rx.waitq);
	} else {
		/* TX */
		ret = i2c_master_send(dev->client, mbo->virt_address,
				      mbo->buffer_length);
		if (ret <= 0) {
			mbo->processed_length = 0;
			mbo->status = MBO_E_INVAL;
		} else {
			mbo->processed_length = mbo->buffer_length;
			mbo->status = MBO_SUCCESS;
		}
		mbo->complete(mbo);
	}

	return 0;
}

/**
 * poison_channel - called from MOST core to poison buffers of a channel
 * @iface: pointer to the interface the channel to be poisoned belongs to
 * @channel_id: corresponding channel ID
 *
 * Return 0 on success, negative on failure.
 *
 * If channel direction is RX, complete the buffers in list with
 * status MBO_E_CLOSE
 */
static int poison_channel(struct most_interface *most_iface,
			  int ch_idx)
{
	struct hdm_i2c *dev = to_hdm(most_iface);
	struct mbo *mbo;

	BUG_ON(ch_idx < 0 || ch_idx >= NUM_CHANNELS);
	BUG_ON(!dev->is_open[ch_idx]);

	dev->is_open[ch_idx] = false;

	if (ch_idx == CH_RX) {
		mutex_lock(&dev->rx.list_mutex);
		while (!list_empty(&dev->rx.list)) {
			mbo = list_first_mbo(&dev->rx.list);
			list_del(&mbo->list);
			mutex_unlock(&dev->rx.list_mutex);

			mbo->processed_length = 0;
			mbo->status = MBO_E_CLOSE;
			mbo->complete(mbo);

			mutex_lock(&dev->rx.list_mutex);
		}
		mutex_unlock(&dev->rx.list_mutex);
		wake_up_interruptible(&dev->rx.waitq);
	}

	return 0;
}

static void request_netinfo(struct most_interface *most_iface,
			    int ch_idx)
{
	pr_info("request_netinfo()\n");
}

static void do_rx_work(struct hdm_i2c *dev)
{
	struct mbo *mbo;
	unsigned char msg[MAX_BUF_SIZE_CONTROL];
	int ret, ch_idx = CH_RX;
	u16 pml, data_size;

	/* Read PML (2 bytes) */
	ret = i2c_master_recv(dev->client, msg, 2);
	if (ret <= 0) {
		pr_err("Failed to receive PML\n");
		return;
	}

	pml = (msg[0] << 8) | msg[1];
	if (!pml)
		return;

	data_size = pml + 2;

	/* Read the whole message, including PML */
	ret = i2c_master_recv(dev->client, msg, data_size);
	if (ret <= 0) {
		pr_err("Failed to receive a Port Message\n");
		return;
	}

	for (;;) {
		/* Conditions to wait for: poisoned channel or free buffer
		 * available for reading
		 */
		if (wait_event_interruptible(dev->rx.waitq,
					     !dev->is_open[ch_idx] ||
					     !list_empty(&dev->rx.list))) {
			pr_err("wait_event_interruptible() failed\n");
			return;
		}

		if (!dev->is_open[ch_idx])
			return;

		mutex_lock(&dev->rx.list_mutex);

		/* list may be empty if poison or remove is called */
		if (!list_empty(&dev->rx.list))
			break;

		mutex_unlock(&dev->rx.list_mutex);
	}

	mbo = list_first_mbo(&dev->rx.list);
	list_del(&mbo->list);
	mutex_unlock(&dev->rx.list_mutex);

	mbo->processed_length = min(data_size, mbo->buffer_length);
	memcpy(mbo->virt_address, msg, mbo->processed_length);
	mbo->status = MBO_SUCCESS;
	mbo->complete(mbo);
}

/**
 * pending_rx_work - Read pending messages through I2C
 * @work: definition of this work item
 *
 * Invoked by the Interrupt Service Routine, most_irq_handler()
 */
static void pending_rx_work(struct work_struct *work)
{
	struct hdm_i2c *dev = container_of(work, struct hdm_i2c, rx.dwork.work);

	do_rx_work(dev);

	if (dev->polling_mode) {
		if (dev->is_open[CH_RX])
			queue_delayed_work(system_power_efficient_wq, &dev->rx.dwork,
					      msecs_to_jiffies(MSEC_PER_SEC
							       / scan_rate));
	} else {
		enable_irq(dev->client->irq);
	}
}

/*
 * most_irq_handler - Interrupt Service Routine
 * @irq: irq number
 * @_dev: private data
 *
 * Schedules a delayed work
 *
 * By default the interrupt line behavior is Active Low. Once an interrupt is
 * generated by the device, until driver clears the interrupt (by reading
 * the PMP message), device keeps the interrupt line in low state. Since i2c
 * read is done in work queue, the interrupt line must be disabled temporarily
 * to avoid ISR being called repeatedly. Re-enable the interrupt in workqueue,
 * after reading the message.
 *
 * Note: If we use the interrupt line in Falling edge mode, there is a
 * possibility to miss interrupts when ISR is getting executed.
 *
 */
static irqreturn_t most_irq_handler(int irq, void *_dev)
{
	struct hdm_i2c *dev = _dev;

	disable_irq_nosync(irq);

	queue_delayed_work(system_power_efficient_wq, &dev->rx.dwork, 0);

	return IRQ_HANDLED;
}

/*
 * i2c_probe - i2c probe handler
 * @client: i2c client device structure
 * @id: i2c client device id
 *
 * Return 0 on success, negative on failure.
 *
 * Register the i2c client device as a MOST interface
 */
static int i2c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct hdm_i2c *dev;
	int ret, i;
	struct kobject *kobj;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	/* ID format: i2c-<bus>-<address> */
	snprintf(dev->name, sizeof(dev->name), "i2c-%d-%04x",
		 client->adapter->nr, client->addr);

	for (i = 0; i < NUM_CHANNELS; i++) {
		dev->is_open[i] = false;
		dev->capabilities[i].data_type = MOST_CH_CONTROL;
		dev->capabilities[i].num_buffers_packet = MAX_BUFFERS_CONTROL;
		dev->capabilities[i].buffer_size_packet = MAX_BUF_SIZE_CONTROL;
	}
	dev->capabilities[CH_RX].direction = MOST_CH_RX;
	dev->capabilities[CH_RX].name_suffix = "rx";
	dev->capabilities[CH_TX].direction = MOST_CH_TX;
	dev->capabilities[CH_TX].name_suffix = "tx";

	dev->most_iface.interface = ITYPE_I2C;
	dev->most_iface.description = dev->name;
	dev->most_iface.num_channels = NUM_CHANNELS;
	dev->most_iface.channel_vector = dev->capabilities;
	dev->most_iface.configure = configure_channel;
	dev->most_iface.enqueue = enqueue;
	dev->most_iface.poison_channel = poison_channel;
	dev->most_iface.request_netinfo = request_netinfo;

	INIT_LIST_HEAD(&dev->rx.list);
	mutex_init(&dev->rx.list_mutex);
	init_waitqueue_head(&dev->rx.waitq);

	INIT_DELAYED_WORK(&dev->rx.dwork, pending_rx_work);

	dev->client = client;
	i2c_set_clientdata(client, dev);

	kobj = most_register_interface(&dev->most_iface);
	if (IS_ERR(kobj)) {
		pr_err("Failed to register i2c as a MOST interface\n");
		kfree(dev);
		return PTR_ERR(kobj);
	}

	dev->polling_mode = polling_req || client->irq <= 0;
	if (!dev->polling_mode) {
		pr_info("Requesting IRQ: %d\n", client->irq);
		ret = request_irq(client->irq, most_irq_handler, 0,
				  client->name, dev);
		if (ret) {
			pr_info("IRQ request failed: %d, falling back to polling\n",
				ret);
			dev->polling_mode = true;
		}
	}

	if (dev->polling_mode)
		pr_info("Using polling at rate: %d times/sec\n", scan_rate);

	return 0;
}

/*
 * i2c_remove - i2c remove handler
 * @client: i2c client device structure
 *
 * Return 0 on success.
 *
 * Unregister the i2c client device as a MOST interface
 */
static int i2c_remove(struct i2c_client *client)
{
	struct hdm_i2c *dev = i2c_get_clientdata(client);
	int i;

	if (!dev->polling_mode)
		free_irq(client->irq, dev);

	most_deregister_interface(&dev->most_iface);

	for (i = 0 ; i < NUM_CHANNELS; i++)
		if (dev->is_open[i])
			poison_channel(&dev->most_iface, i);
	cancel_delayed_work_sync(&dev->rx.dwork);
	kfree(dev);

	return 0;
}

static const struct i2c_device_id i2c_id[] = {
	{ "most_i2c", 0 },
	{ }, /* Terminating entry */
};

MODULE_DEVICE_TABLE(i2c, i2c_id);

static struct i2c_driver i2c_driver = {
	.driver = {
		.name = "hdm_i2c",
	},
	.probe = i2c_probe,
	.remove = i2c_remove,
	.id_table = i2c_id,
};

module_i2c_driver(i2c_driver);

MODULE_AUTHOR("Jain Roy Ambi <JainRoy.Ambi@microchip.com>");
MODULE_AUTHOR("Andrey Shvetsov <andrey.shvetsov@k2l.de>");
MODULE_DESCRIPTION("I2C Hardware Dependent Module");
MODULE_LICENSE("GPL");
