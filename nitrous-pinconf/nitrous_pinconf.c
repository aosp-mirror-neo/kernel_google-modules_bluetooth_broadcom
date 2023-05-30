// SPDX-License-Identifier: GPL-2.0
/*
 * Bluetooth pinconf driver
 *
 * Copyright 2015-2023 Google LLC.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/property.h>
struct nitrous_bt_pinconf {
	struct device *dev;
	struct pinctrl *pinctrls;
	struct pinctrl_state *pinctrl_default_state;
	struct pinctrl_state *pinctrl_quiescence_state;
	struct pinctrl_state *pinctrl_active_state;
	struct proc_dir_entry *nbq_dir;
	struct proc_dir_entry *quiescence_entry;
	bool is_quiescence_enabled;           				/* driver forces bt pad conf in safe quiescencestate */
	//bool bt_resumed; 									/* Notifier based */
};

static int nitrous_bt_quiescence_proc_show(struct seq_file *m, void *v)
{
	struct nitrous_bt_pinconf *nbq = m->private;
	seq_printf(m, "nitrous_bt_quiescence: %s\n",  nbq->is_quiescence_enabled ? "Enabled" : "Disabled");
	return 0;
}

static int nitrous_bt_quiescence_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, nitrous_bt_quiescence_proc_show, PDE_DATA(inode));
}

static ssize_t nitrous_bt_quiescence_proc_write(struct file *file, const char *buf,
	size_t count, loff_t *pos)
{
	struct nitrous_bt_pinconf *nbq = PDE_DATA(file_inode(file));
	char lbuf[4];
	int rc = 0;

	if (count >= sizeof(lbuf))
		count = sizeof(lbuf) - 1;
	if (copy_from_user(lbuf, buf, count))
		return -EFAULT;

	if (lbuf[0] == '1') {
		dev_info(nbq->dev, "Enable : BT Quiescence\n");
		nbq->is_quiescence_enabled = true;

		// set pinconf quiescence state
		if (!IS_ERR_OR_NULL(nbq->pinctrl_quiescence_state)) {
			rc = pinctrl_select_state(nbq->pinctrls,
					  nbq->pinctrl_quiescence_state);
			if (unlikely(rc))
				dev_warn(nbq->dev, "Can't set quiescence pinctrl state\n");
		}
	} else {
		dev_info(nbq->dev, "Disable : BT Quiescence\n");
		nbq->is_quiescence_enabled = false;
		// restore state on Quiescence disable
		if (!IS_ERR_OR_NULL(nbq->pinctrl_active_state)) {
			rc = pinctrl_select_state(nbq->pinctrls,
					  nbq->pinctrl_active_state);
			if (unlikely(rc))
				dev_warn(nbq->dev, "Can't reset active pinctrl state\n");
		}
	}
	return count;
}

static const struct proc_ops nitrous_bt_quiescence_proc_readwrite_fops = {
	.proc_open	= nitrous_bt_quiescence_proc_open,
	.proc_read	= seq_read,
	.proc_write	= nitrous_bt_quiescence_proc_write,
	.proc_release	= single_release,
};

static void nitrous_bt_quiescence_remove_proc_entries(struct nitrous_bt_pinconf *nbq)
{
	if (nbq->nbq_dir == NULL)
		return;
	remove_proc_entry("quiescence", nbq->nbq_dir);
	remove_proc_entry("nbq", 0);
}

static int nitrous_bt_pinconf_init(struct nitrous_bt_pinconf *nbq)
{
	int rc;

	nbq->nbq_dir = proc_mkdir("nbq", NULL);
	if (nbq->nbq_dir == NULL) {
		dev_err(nbq->dev, "Unable to create/reuse /proc/nbq directory");
		rc = -ENOMEM;
		goto fail;
	}
	nbq->quiescence_entry = proc_create_data("quiescence", (S_IRUSR | S_IRGRP | S_IWUSR), nbq->nbq_dir,
				 &nitrous_bt_quiescence_proc_readwrite_fops, nbq);
	if (nbq->quiescence_entry == NULL) {
		dev_err(nbq->dev, "Unable to create /proc/nbq/quiescence entry");
		rc = -ENOMEM;
		goto fail;
	}
	return 0;
fail:
	nitrous_bt_quiescence_remove_proc_entries(nbq);
	return rc;
}

static int nitrous_bt_pinconf_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nitrous_bt_pinconf *nbq;
	int rc = 0;

	nbq = devm_kzalloc(dev, sizeof(struct nitrous_bt_pinconf), GFP_KERNEL);
	if (!nbq)
		return -ENOMEM;

	nbq->dev = dev;
	dev_dbg(nbq->dev, "probe:\n");

	rc = nitrous_bt_pinconf_init(nbq);
	if (unlikely(rc))
		goto err_nbq_init;

	nbq->pinctrls = devm_pinctrl_get(nbq->dev);
	if (IS_ERR(nbq->pinctrls)) {
		dev_warn(nbq->dev, "Can't get pinctrl\n");
	} else {
		nbq->pinctrl_quiescence_state =
			pinctrl_lookup_state(nbq->pinctrls, "quiescence");
		if (IS_ERR(nbq->pinctrl_quiescence_state))
			dev_warn(nbq->dev, "Can't get quiescence pinctrl state\n");

		nbq->pinctrl_active_state =
			pinctrl_lookup_state(nbq->pinctrls, "active");
		if (IS_ERR(nbq->pinctrl_active_state))
			dev_warn(nbq->dev, "Can't get active pinctrl state\n");

		nbq->pinctrl_default_state =
			pinctrl_lookup_state(nbq->pinctrls, "default");
		if (IS_ERR(nbq->pinctrl_default_state))
			dev_warn(nbq->dev, "Can't get default pinctrl state\n");

		if (!IS_ERR_OR_NULL(nbq->pinctrl_active_state)) {
			rc = pinctrl_select_state(nbq->pinctrls,
					  nbq->pinctrl_active_state);
			if (unlikely(rc))
				dev_warn(nbq->dev, "Can't set default pinctrl state\n");
		}
	}

	platform_set_drvdata(pdev, nbq);
	return rc;

err_nbq_init:
	nitrous_bt_quiescence_remove_proc_entries(nbq);
	devm_kfree(dev, nbq);
	return rc;
}

static int nitrous_bt_pinconf_remove(struct platform_device *pdev)
{
	struct nitrous_bt_pinconf *nbq = platform_get_drvdata(pdev);

	if (!nbq) {
		return -EINVAL;
	}
	nitrous_bt_quiescence_remove_proc_entries(nbq);
	devm_kfree(&pdev->dev, nbq);
	return 0;
}

static struct of_device_id nitrous_bt_pinconf_match_table[] = {
	{ .compatible = "goog,nitrous-bt-pinconf" },
	{}
};

static struct platform_driver nitrous_bt_pinconf_platform_driver = {
	.probe = nitrous_bt_pinconf_probe,
	.remove =  nitrous_bt_pinconf_remove,
	.driver = {
		.name = "nitrous_bt_pinconf",
		.owner = THIS_MODULE,
		.of_match_table = nitrous_bt_pinconf_match_table,
	},
};

static int __init nitrous_bt_pinconf_module_init(void)
{
	return platform_driver_register(&nitrous_bt_pinconf_platform_driver);
}

static void __exit nitrous_bt_pinconf_module_exit(void)
{
	platform_driver_unregister(&nitrous_bt_pinconf_platform_driver);
}

module_init(nitrous_bt_pinconf_module_init);
module_exit(nitrous_bt_pinconf_module_exit);
MODULE_DESCRIPTION("Nitrous bt pinconf driver");
MODULE_AUTHOR("Google");
MODULE_LICENSE("GPL");