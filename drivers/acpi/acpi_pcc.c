// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: Sudeep Holla <sudeep.holla@arm.com>
 * Copyright 2021 Arm Limited
 *
 * The PCC Address Space also referred as PCC Operation Region pertains to the
 * region of PCC subspace that succeeds the PCC signature. The PCC Operation
 * Region works in conjunction with the PCC Table(Platform Communications
 * Channel Table). PCC subspaces that are marked for use as PCC Operation
 * Regions must not be used as PCC subspaces for the standard ACPI features
 * such as CPPC, RASF, PDTT and MPST. These standard features must always use
 * the PCC Table instead.
 *
 * This driver sets up the PCC Address Space and installs an handler to enable
 * handling of PCC OpRegion in the firmware.
 *
 */
#include <linux/kernel.h>
#include <linux/acpi.h>
#include <linux/completion.h>
#include <linux/idr.h>
#include <linux/io.h>

#include <acpi/pcc.h>

/*
 * Arbitrary retries in case the remote processor is slow to respond
 * to PCC commands
 */
#define PCC_CMD_WAIT_RETRIES_NUM	500ULL

struct pcc_data {
	struct pcc_mbox_chan *pcc_chan;
	void __iomem *pcc_comm_addr;
	struct completion done;
	struct mbox_client cl;
	struct acpi_pcc_info ctx;
};

static struct acpi_pcc_info pcc_ctx;

static void pcc_rx_callback(struct mbox_client *cl, void *m)
{
	struct pcc_data *data = container_of(cl, struct pcc_data, cl);

	complete(&data->done);
}

/* 初始化PCC通道的硬件资源并设置上下文 */
static acpi_status
acpi_pcc_address_space_setup(acpi_handle region_handle, u32 function,
			     void *handler_context,  void **region_context)
{
	struct pcc_data *data;
	struct acpi_pcc_info *ctx = handler_context;
	struct pcc_mbox_chan *pcc_chan;
	static acpi_status ret;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return AE_NO_MEMORY;

	data->cl.rx_callback = pcc_rx_callback;
	data->cl.knows_txdone = true;
	data->ctx.length = ctx->length;
	data->ctx.subspace_id = ctx->subspace_id;
	data->ctx.internal_buffer = ctx->internal_buffer;

	init_completion(&data->done);
	data->pcc_chan = pcc_mbox_request_channel(&data->cl, ctx->subspace_id);
	if (IS_ERR(data->pcc_chan)) {
		pr_err("Failed to find PCC channel for subspace %d\n",
		       ctx->subspace_id);
		ret = AE_NOT_FOUND;
		goto err_free_data;
	}

	pcc_chan = data->pcc_chan;
	if (!pcc_chan->mchan->mbox->txdone_irq) {
		pr_err("This channel-%d does not support interrupt.\n",
		       ctx->subspace_id);
		ret = AE_SUPPORT;
		goto err_free_channel;
	}
	data->pcc_comm_addr = acpi_os_ioremap(pcc_chan->shmem_base_addr,
					      pcc_chan->shmem_size);
	if (!data->pcc_comm_addr) {
		pr_err("Failed to ioremap PCC comm region mem for %d\n",
		       ctx->subspace_id);
		ret = AE_NO_MEMORY;
		goto err_free_channel;
	}

	*region_context = data;
	return AE_OK;

err_free_channel:
	pcc_mbox_free_channel(data->pcc_chan);
err_free_data:
	kfree(data);

	return ret;
}

static acpi_status
acpi_pcc_address_space_handler(u32 function, acpi_physical_address addr,
			       u32 bits, acpi_integer *value,
			       void *handler_context, void *region_context)
{
	int ret;
	struct pcc_data *data = region_context;
	u64 usecs_lat;

	reinit_completion(&data->done);

	/* Write to Shared Memory */
	memcpy_toio(data->pcc_comm_addr, (void *)value, data->ctx.length);

	ret = mbox_send_message(data->pcc_chan->mchan, NULL);
	if (ret < 0)
		return AE_ERROR;

	/*
	 * pcc_chan->latency is just a Nominal value. In reality the remote
	 * processor could be much slower to reply. So add an arbitrary
	 * amount of wait on top of Nominal.
	 */
	usecs_lat = PCC_CMD_WAIT_RETRIES_NUM * data->pcc_chan->latency;
	ret = wait_for_completion_timeout(&data->done,
						usecs_to_jiffies(usecs_lat));
	if (ret == 0) {
		pr_err("PCC command executed timeout!\n");
		return AE_TIME;
	}

	mbox_chan_txdone(data->pcc_chan->mchan, ret);

	memcpy_fromio(value, data->pcc_comm_addr, data->ctx.length);

	return AE_OK;
}

/* 向ACPI框架注册PCC通信通道的地址空间处理程序 */
void __init acpi_init_pcc(void)
{
	acpi_status status;
	/* 向ACPI框架注册PCC通信通道的地址空间处理程序 */
	status = acpi_install_address_space_handler(ACPI_ROOT_OBJECT,//表示ACPI命名空间的根对象,处理程序将绑定到根对象下的所有OperationRegion类型为Platform Communication的区域。
						    ACPI_ADR_SPACE_PLATFORM_COMM,//地址空间类型标识符，对应ACPI规范定义的平台通信空间（PCC）
						    &acpi_pcc_address_space_handler,//地址空间操作函数指针，处理对PCC空间的读/写请求（如访问PCC寄存器）
						    &acpi_pcc_address_space_setup,//初始化函数指针，在安装时调用以设置PCC上下文（如分配资源、验证硬件）
						    &pcc_ctx);//用户提供的上下文指针，传递给处理函数和初始化函数，用于存储PCC的私有数据（如寄存器基址、状态等）。

	if (ACPI_FAILURE(status))//如果安装失败（如ACPI框架未支持PCC或资源不足），输出告警信息
		pr_alert("OperationRegion handler could not be installed\n");
}
