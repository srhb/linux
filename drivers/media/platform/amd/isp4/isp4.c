// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include <linux/pm_runtime.h>
#include <linux/vmalloc.h>
#include <media/v4l2-ioctl.h>

#include "isp4.h"

#define VIDEO_BUF_NUM 5

#define ISP4_DRV_NAME "amd_isp_capture"

const char *isp4_irq_name[] = {
	"isp_irq_stream1",
	"isp_irq_global"
};

/* interrupt num */
static const u32 isp4_ringbuf_interrupt_num[] = {
	0, /* ISP_4_1__SRCID__ISP_RINGBUFFER_WPT9 */
	4, /* ISP_4_1__SRCID__ISP_RINGBUFFER_WPT12 */
};

static irqreturn_t isp4_irq_handler(int irq, void *arg)
{
	return IRQ_HANDLED;
}

static int isp4_capture_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct isp4_device *isp_dev;
	int i, irq, ret;

	isp_dev = devm_kzalloc(dev, sizeof(*isp_dev), GFP_KERNEL);
	if (!isp_dev)
		return -ENOMEM;

	isp_dev->pdev = pdev;
	dev->init_name = ISP4_DRV_NAME;

	for (i = 0; i < ARRAY_SIZE(isp4_ringbuf_interrupt_num); i++) {
		irq = platform_get_irq(pdev, isp4_ringbuf_interrupt_num[i]);
		if (irq < 0)
			return dev_err_probe(dev, irq,
					     "fail to get irq %d\n",
					     isp4_ringbuf_interrupt_num[i]);
		ret = devm_request_irq(dev, irq, isp4_irq_handler, 0,
				       isp4_irq_name[i], dev);
		if (ret)
			return dev_err_probe(dev, ret, "fail to req irq %d\n",
					     irq);
	}

	/* Link the media device within the v4l2_device */
	isp_dev->v4l2_dev.mdev = &isp_dev->mdev;

	/* Initialize media device */
	strscpy(isp_dev->mdev.model, "amd_isp41_mdev",
		sizeof(isp_dev->mdev.model));
	snprintf(isp_dev->mdev.bus_info, sizeof(isp_dev->mdev.bus_info),
		 "platform:%s", ISP4_DRV_NAME);
	isp_dev->mdev.dev = dev;
	media_device_init(&isp_dev->mdev);

	/* register v4l2 device */
	snprintf(isp_dev->v4l2_dev.name, sizeof(isp_dev->v4l2_dev.name),
		 "AMD-V4L2-ROOT");
	ret = v4l2_device_register(dev, &isp_dev->v4l2_dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "fail register v4l2 device\n");

	ret = media_device_register(&isp_dev->mdev);
	if (ret) {
		dev_err(dev, "fail to register media device %d\n", ret);
		goto err_unreg_v4l2;
	}

	platform_set_drvdata(pdev, isp_dev);

	pm_runtime_set_suspended(dev);
	pm_runtime_enable(dev);

	return 0;

err_unreg_v4l2:
	v4l2_device_unregister(&isp_dev->v4l2_dev);

	return dev_err_probe(dev, ret, "isp probe fail\n");
}

static void isp4_capture_remove(struct platform_device *pdev)
{
	struct isp4_device *isp_dev = platform_get_drvdata(pdev);

	media_device_unregister(&isp_dev->mdev);
	v4l2_device_unregister(&isp_dev->v4l2_dev);
}

static struct platform_driver isp4_capture_drv = {
	.probe = isp4_capture_probe,
	.remove = isp4_capture_remove,
	.driver = {
		.name = ISP4_DRV_NAME,
		.owner = THIS_MODULE,
	}
};

module_platform_driver(isp4_capture_drv);

MODULE_ALIAS("platform:" ISP4_DRV_NAME);
MODULE_IMPORT_NS("DMA_BUF");

MODULE_DESCRIPTION("AMD ISP4 Driver");
MODULE_AUTHOR("Bin Du <bin.du@amd.com>");
MODULE_AUTHOR("Pratap Nirujogi <pratap.nirujogi@amd.com>");
MODULE_LICENSE("GPL");
