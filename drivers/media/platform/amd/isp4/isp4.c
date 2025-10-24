// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include <linux/irq.h>
#include <linux/pm_runtime.h>
#include <linux/vmalloc.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ioctl.h>

#include "isp4.h"
#include "isp4_debug.h"
#include "isp4_hw_reg.h"

#define ISP4_DRV_NAME "amd_isp_capture"
#define ISP4_FW_RESP_RB_IRQ_STATUS_MASK \
	(ISP_SYS_INT0_STATUS__SYS_INT_RINGBUFFER_WPT9_INT_MASK  | \
	 ISP_SYS_INT0_STATUS__SYS_INT_RINGBUFFER_WPT12_INT_MASK)

const char *isp4_irq_name[] = {
	"isp_irq_global",
	"isp_irq_stream1"
};

const u32 isp4_irq_status_mask[ISP4SD_MAX_FW_RESP_STREAM_NUM] = {
	/* global response */
	ISP_SYS_INT0_STATUS__SYS_INT_RINGBUFFER_WPT12_INT_MASK,
	/* stream 1 response */
	ISP_SYS_INT0_STATUS__SYS_INT_RINGBUFFER_WPT9_INT_MASK
};

const u32 isp4_irq_ack_mask[ISP4SD_MAX_FW_RESP_STREAM_NUM] = {
	/* global ack */
	ISP_SYS_INT0_ACK__SYS_INT_RINGBUFFER_WPT12_ACK_MASK,
	/* stream 1 ack */
	ISP_SYS_INT0_ACK__SYS_INT_RINGBUFFER_WPT9_ACK_MASK
};

/* irq num, the irq order is aligend with the isp4_subdev.fw_resp_thread order */
static const u32 isp4_ringbuf_interrupt_num[ISP4SD_MAX_FW_RESP_STREAM_NUM] = {
	4, /* ISP_4_1__SRCID__ISP_RINGBUFFER_WPT12 */
	0  /* ISP_4_1__SRCID__ISP_RINGBUFFER_WPT9 */
};

static void isp4_wake_up_resp_thread(struct isp4_subdev *isp_subdev, u32 index)
{
	if (isp_subdev && index < ISP4SD_MAX_FW_RESP_STREAM_NUM) {
		struct isp4sd_thread_handler *thread_ctx = &isp_subdev->fw_resp_thread[index];

		thread_ctx->wq_cond = 1;
		wake_up_interruptible(&thread_ctx->waitq);
	}
}

static void isp4_resp_interrupt_notify(struct isp4_subdev *isp_subdev, u32 intr_status)
{
	u32 intr_ack = 0;

	for (size_t i = 0; i < ARRAY_SIZE(isp4_irq_status_mask); i++) {
		if (intr_status & isp4_irq_status_mask[i]) {
			disable_irq_nosync(isp_subdev->irq[i]);
			isp4_wake_up_resp_thread(isp_subdev, i);

			intr_ack |= isp4_irq_ack_mask[i];
		}
	}

	/* clear ISP_SYS interrupts */
	isp4hw_wreg(ISP4_GET_ISP_REG_BASE(isp_subdev), ISP_SYS_INT0_ACK, intr_ack);
}

static irqreturn_t isp4_irq_handler(int irq, void *arg)
{
	struct isp4_device *isp_dev = arg;
	struct isp4_subdev *isp_subdev;
	u32 isp_sys_irq_status;
	u32 r1;

	isp_subdev = &isp_dev->isp_subdev;
	/* check ISP_SYS interrupts status */
	r1 = isp4hw_rreg(ISP4_GET_ISP_REG_BASE(isp_subdev), ISP_SYS_INT0_STATUS);

	isp_sys_irq_status = r1 & ISP4_FW_RESP_RB_IRQ_STATUS_MASK;

	isp4_resp_interrupt_notify(isp_subdev, isp_sys_irq_status);

	return IRQ_HANDLED;
}

static int isp4_capture_probe(struct platform_device *pdev)
{
	int irq[ISP4SD_MAX_FW_RESP_STREAM_NUM];
	struct device *dev = &pdev->dev;
	struct isp4_subdev *isp_subdev;
	struct isp4_device *isp_dev;
	size_t i;
	int ret;

	isp_dev = devm_kzalloc(dev, sizeof(*isp_dev), GFP_KERNEL);
	if (!isp_dev)
		return -ENOMEM;

	isp_dev->pdev = pdev;
	dev->init_name = ISP4_DRV_NAME;

	isp_subdev = &isp_dev->isp_subdev;
	isp_subdev->mmio = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(isp_subdev->mmio))
		return dev_err_probe(dev, PTR_ERR(isp_subdev->mmio), "isp ioremap fail\n");

	for (i = 0; i < ARRAY_SIZE(isp4_ringbuf_interrupt_num); i++) {
		irq[i] = platform_get_irq(pdev, isp4_ringbuf_interrupt_num[i]);
		if (irq[i] < 0)
			return dev_err_probe(dev, irq[i], "fail to get irq %d\n",
					     isp4_ringbuf_interrupt_num[i]);

		irq_set_status_flags(irq[i], IRQ_NOAUTOEN);
		ret = devm_request_irq(dev, irq[i], isp4_irq_handler, 0, isp4_irq_name[i],
				       isp_dev);
		if (ret)
			return dev_err_probe(dev, ret, "fail to req irq %d\n", irq[i]);
	}

	/* Link the media device within the v4l2_device */
	isp_dev->v4l2_dev.mdev = &isp_dev->mdev;

	/* Initialize media device */
	strscpy(isp_dev->mdev.model, "amd_isp41_mdev", sizeof(isp_dev->mdev.model));
	snprintf(isp_dev->mdev.bus_info, sizeof(isp_dev->mdev.bus_info),
		 "platform:%s", ISP4_DRV_NAME);
	isp_dev->mdev.dev = dev;
	media_device_init(&isp_dev->mdev);

	pm_runtime_set_suspended(dev);
	pm_runtime_enable(dev);
	/* register v4l2 device */
	snprintf(isp_dev->v4l2_dev.name, sizeof(isp_dev->v4l2_dev.name),
		 "AMD-V4L2-ROOT");
	ret = v4l2_device_register(dev, &isp_dev->v4l2_dev);
	if (ret)
		return dev_err_probe(dev, ret, "fail register v4l2 device\n");

	ret = isp4sd_init(&isp_dev->isp_subdev, &isp_dev->v4l2_dev, irq);
	if (ret) {
		dev_err(dev, "fail init isp4 sub dev %d\n", ret);
		goto err_unreg_v4l2;
	}

	ret = media_create_pad_link(&isp_dev->isp_subdev.sdev.entity,
				    0, &isp_dev->isp_subdev.isp_vdev.vdev.entity,
				    0,
				    MEDIA_LNK_FL_ENABLED |
				    MEDIA_LNK_FL_IMMUTABLE);
	if (ret) {
		dev_err(dev, "fail to create pad link %d\n", ret);
		goto err_isp4_deinit;
	}

	ret = media_device_register(&isp_dev->mdev);
	if (ret) {
		dev_err(dev, "fail to register media device %d\n", ret);
		goto err_isp4_deinit;
	}

	platform_set_drvdata(pdev, isp_dev);
	isp_debugfs_create(isp_dev);

	return 0;

err_isp4_deinit:
	isp4sd_deinit(&isp_dev->isp_subdev);
err_unreg_v4l2:
	v4l2_device_unregister(&isp_dev->v4l2_dev);
	media_device_cleanup(&isp_dev->mdev);

	return dev_err_probe(dev, ret, "isp probe fail\n");
}

static void isp4_capture_remove(struct platform_device *pdev)
{
	struct isp4_device *isp_dev = platform_get_drvdata(pdev);

	isp_debugfs_remove(isp_dev);

	media_device_unregister(&isp_dev->mdev);
	isp4sd_deinit(&isp_dev->isp_subdev);
	v4l2_device_unregister(&isp_dev->v4l2_dev);
	media_device_cleanup(&isp_dev->mdev);
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
