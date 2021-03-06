/* arch/arm/mach-msm/htc_sdservice.c
 *
 * Copyright (C) 2012 HTC Corporation.
 * Author: Tommy.Chiu <tommy_chiu@htc.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/module.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>

#ifndef CONFIG_ARCH_MSM7X30
#include <mach/scm.h>
#else	
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <asm/io.h>
#include <asm/system.h>
#include <linux/semaphore.h>
#include <mach/msm_rpcrouter.h>
#include <mach/oem_rapi_client.h>

#define OEM_RAPI_PROG  0x3000006B
#define OEM_RAPI_VERS  0x00010001

#define OEM_RAPI_NULL_PROC                        0
#define OEM_RAPI_RPC_GLUE_CODE_INFO_REMOTE_PROC   1
#define OEM_RAPI_STREAMING_FUNCTION_PROC          2

#define OEM_RAPI_CLIENT_MAX_OUT_BUFF_SIZE 32
#endif	

#define DEVICE_NAME "htc_sdservice"

#define HTC_SDKEY_LEN 32
#define HTC_IOCTL_SDSERVICE 0x9527

#define HTC_SDSERVICE_DEBUG	0
#undef PDEBUG
#if HTC_SDSERVICE_DEBUG
#define PDEBUG(fmt, args...) printk(KERN_INFO "%s(%i, %s): " fmt "\n", \
		__func__, current->pid, current->comm, ## args)
#else
#define PDEBUG(fmt, args...) do {} while (0)
#endif 

#undef PERR
#define PERR(fmt, args...) printk(KERN_ERR "%s(%i, %s): " fmt "\n", \
		__func__, current->pid, current->comm, ## args)

static int htc_sdservice_major;
static struct class *htc_sdservice_class;
static const struct file_operations htc_sdservice_fops;

static unsigned char *htc_sdkey;

typedef struct _htc_sdservice_msg_s{
	int func;
	int offset;
	unsigned char *req_buf;
	int req_len;
	unsigned char *resp_buf;
	int resp_len;
} htc_sdservice_msg_s;

enum {
		HTC_SD_KEY_ENCRYPT = 0x33,
		HTC_SD_KEY_DECRYPT,
};

#ifdef CONFIG_ARCH_MSM7X30
static struct msm_rpc_client *rpc_client;
static DEFINE_MUTEX(oem_rapi_client_lock);
int oem_rapi_client_cb(struct msm_rpc_client *client,
			      struct rpc_request_hdr *req,
			      struct msm_rpc_xdr *xdr)
{
	uint32_t cb_id, accept_status;
	int rc;
	void *cb_func;
	uint32_t temp;

	struct oem_rapi_client_streaming_func_cb_arg arg;
	struct oem_rapi_client_streaming_func_cb_ret ret;

	arg.input = NULL;
	ret.out_len = NULL;
	ret.output = NULL;

	xdr_recv_uint32(xdr, &cb_id);                    
	xdr_recv_uint32(xdr, &arg.event);                
	xdr_recv_uint32(xdr, (uint32_t *)(&arg.handle)); 
	xdr_recv_uint32(xdr, &arg.in_len);               
	xdr_recv_bytes(xdr, (void **)&arg.input, &temp); 
	xdr_recv_uint32(xdr, &arg.out_len_valid);        
	if (arg.out_len_valid) {
		ret.out_len = kmalloc(sizeof(*ret.out_len), GFP_KERNEL);
		if (!ret.out_len) {
			accept_status = RPC_ACCEPTSTAT_SYSTEM_ERR;
			goto oem_rapi_send_ack;
		}
	}

	xdr_recv_uint32(xdr, &arg.output_valid);         
	if (arg.output_valid) {
		xdr_recv_uint32(xdr, &arg.output_size);  

		ret.output = kmalloc(arg.output_size, GFP_KERNEL);
		if (!ret.output) {
			accept_status = RPC_ACCEPTSTAT_SYSTEM_ERR;
			goto oem_rapi_send_ack;
		}
	}

	cb_func = msm_rpc_get_cb_func(client, cb_id);
	if (cb_func) {
		rc = ((int (*)(struct oem_rapi_client_streaming_func_cb_arg *,
			       struct oem_rapi_client_streaming_func_cb_ret *))
		      cb_func)(&arg, &ret);
		if (rc)
			accept_status = RPC_ACCEPTSTAT_SYSTEM_ERR;
		else
			accept_status = RPC_ACCEPTSTAT_SUCCESS;
	} else
		accept_status = RPC_ACCEPTSTAT_SYSTEM_ERR;

 oem_rapi_send_ack:
	xdr_start_accepted_reply(xdr, accept_status);

	if (accept_status == RPC_ACCEPTSTAT_SUCCESS) {
		uint32_t temp = sizeof(uint32_t);
		xdr_send_pointer(xdr, (void **)&(ret.out_len), temp,
				 xdr_send_uint32);

		
		if (ret.output && ret.out_len)
			xdr_send_bytes(xdr, (const void **)&ret.output,
					     ret.out_len);
		else {
			temp = 0;
			xdr_send_uint32(xdr, &temp);
		}
	}
	rc = xdr_send_msg(xdr);
	if (rc)
		pr_err("%s: sending reply failed: %d\n", __func__, rc);

	kfree(arg.input);
	kfree(ret.out_len);
	kfree(ret.output);

	return 0;
}

int oem_rapi_client_streaming_function_arg(struct msm_rpc_client *client,
						  struct msm_rpc_xdr *xdr,
						  void *data)
{
	int cb_id;
	struct oem_rapi_client_streaming_func_arg *arg = data;

	cb_id = msm_rpc_add_cb_func(client, (void *)arg->cb_func);
	if ((cb_id < 0) && (cb_id != MSM_RPC_CLIENT_NULL_CB_ID))
		return cb_id;

	xdr_send_uint32(xdr, &arg->event);                
	xdr_send_uint32(xdr, &cb_id);                     
	xdr_send_uint32(xdr, (uint32_t *)(&arg->handle)); 
	xdr_send_uint32(xdr, &arg->in_len);               
	xdr_send_bytes(xdr, (const void **)&arg->input,
			     &arg->in_len);                     
	xdr_send_uint32(xdr, &arg->out_len_valid);        
	xdr_send_uint32(xdr, &arg->output_valid);         

	
	if (arg->output_valid)
		xdr_send_uint32(xdr, &arg->output_size);

	return 0;
}

int oem_rapi_client_streaming_function_ret(struct msm_rpc_client *client,
						  struct msm_rpc_xdr *xdr,
						  void *data)
{
	struct oem_rapi_client_streaming_func_ret *ret = data;
	uint32_t temp;

	
	xdr_recv_pointer(xdr, (void **)&(ret->out_len), sizeof(uint32_t),
			 xdr_recv_uint32);

	
	if (ret->out_len && *ret->out_len)
		xdr_recv_bytes(xdr, (void **)&ret->output, &temp);

	return 0;
}

static uint32_t open_count;
int oem_rapi_client_streaming_function2(
	struct msm_rpc_client *client,
	struct oem_rapi_client_streaming_func_arg *arg,
	struct oem_rapi_client_streaming_func_ret *ret)
{
	return msm_rpc_client_req2(client,
				   OEM_RAPI_STREAMING_FUNCTION_PROC,
				   oem_rapi_client_streaming_function_arg, arg,
				   oem_rapi_client_streaming_function_ret,
				   ret, -1);
}
EXPORT_SYMBOL(oem_rapi_client_streaming_function2);

int oem_rapi_client_close2(void)
{
	mutex_lock(&oem_rapi_client_lock);
	if (open_count > 0) {
		if (--open_count == 0) {
			msm_rpc_unregister_client(rpc_client);
			pr_info("%s: disconnected from remote oem rapi server\n",
				__func__);
		}
	}
	mutex_unlock(&oem_rapi_client_lock);
	return 0;
}
EXPORT_SYMBOL(oem_rapi_client_close2);

struct msm_rpc_client *oem_rapi_client_init2(void)
{
	mutex_lock(&oem_rapi_client_lock);
	if (open_count == 0) {
		rpc_client = msm_rpc_register_client2("oemrapiclient",
						      OEM_RAPI_PROG,
						      OEM_RAPI_VERS, 0,
						      oem_rapi_client_cb);
		if (!IS_ERR(rpc_client))
			open_count++;
	} else {
		
		open_count++;
	}
	mutex_unlock(&oem_rapi_client_lock);
	return rpc_client;
}
EXPORT_SYMBOL(oem_rapi_client_init2);

ssize_t oem_rapi_pack_send(unsigned int operation, char *buf, size_t size)
{
	int ret_rpc = 0;
	struct oem_rapi_client_streaming_func_arg arg;
	struct oem_rapi_client_streaming_func_ret ret;

	printk(KERN_INFO "oem_rapi_pack start:\n");
	arg.event = operation;
	arg.cb_func = NULL;
	arg.handle = (void *)0;
	arg.in_len = OEM_RAPI_CLIENT_MAX_OUT_BUFF_SIZE;
	arg.input = buf;
	arg.out_len_valid = 1;
	arg.output_valid = 1;
	arg.output_size = OEM_RAPI_CLIENT_MAX_OUT_BUFF_SIZE;
	ret.out_len = NULL;
	ret.output = NULL;

	ret_rpc = oem_rapi_client_streaming_function2(rpc_client, &arg, &ret);
	if (ret_rpc) {
		printk(KERN_ERR "%s: Send data from modem failed: %d\n", __func__, ret_rpc);
		return -EFAULT;
	}
	printk(KERN_INFO "%s: Data sent to modem %s\n", __func__, buf);
	if(ret.output)
		memcpy(buf, ret.output, OEM_RAPI_CLIENT_MAX_OUT_BUFF_SIZE);
	else{
		printk(KERN_ERR "%s: Receive data from modem failed\n", __func__);
		return -EFAULT;
	}

	return 0;
}
#endif 

static long htc_sdservice_ioctl(struct file *file, unsigned int command, unsigned long arg)
{
	htc_sdservice_msg_s hmsg;
	int ret = 0;

	PDEBUG("command = %x\n", command);
	switch (command) {
	case HTC_IOCTL_SDSERVICE:
		if (copy_from_user(&hmsg, (void __user *)arg, sizeof(hmsg))) {
			PERR("copy_from_user error (msg)");
			return -EFAULT;
		}
#ifdef CONFIG_ARCH_MSM7X30
		oem_rapi_client_init2();
#endif 
		PDEBUG("func = %x\n", hmsg.func);
		switch (hmsg.func) {
		case HTC_SD_KEY_ENCRYPT:
			if ((hmsg.req_buf == NULL) || (hmsg.req_len != HTC_SDKEY_LEN)) {
				PERR("invalid arguments");
				return -EFAULT;
			}
			if (copy_from_user(htc_sdkey, (void __user *)hmsg.req_buf, hmsg.req_len)) {
				PERR("copy_from_user error (sdkey)");
				return -EFAULT;
			}
#ifdef CONFIG_ARCH_MSM7X30
			ret = oem_rapi_pack_send(OEM_RAPI_CLIENT_EVENT_SDSERVICE_ENC, htc_sdkey, hmsg.req_len);
			oem_rapi_client_close2();
#else
			ret = secure_access_item(0, HTC_SD_KEY_ENCRYPT, hmsg.req_len, htc_sdkey);
#endif	
			if (ret)
				PERR("Encrypt SD key fail (%d)\n", ret);

			if (copy_to_user((void __user *)hmsg.resp_buf, htc_sdkey, hmsg.req_len)) {
				PERR("copy_to_user error (sdkey)");
				return -EFAULT;
			}
			break;

		case HTC_SD_KEY_DECRYPT:
			if ((hmsg.req_buf == NULL) || (hmsg.req_len != HTC_SDKEY_LEN)) {
				PERR("invalid arguments");
				return -EFAULT;
			}
			if (copy_from_user(htc_sdkey, (void __user *)hmsg.req_buf, hmsg.req_len)) {
				PERR("copy_from_user error (sdkey)");
				return -EFAULT;
			}
#ifdef CONFIG_ARCH_MSM7X30
			ret = oem_rapi_pack_send(OEM_RAPI_CLIENT_EVENT_SDSERVICE_DEC, htc_sdkey, hmsg.req_len);
			oem_rapi_client_close2();
#else
			ret = secure_access_item(0, HTC_SD_KEY_DECRYPT, hmsg.req_len, htc_sdkey);
#endif	
			if (ret)
				PERR("Encrypt SD key fail (%d)\n", ret);

			if (copy_to_user((void __user *)hmsg.resp_buf, htc_sdkey, hmsg.req_len)) {
				PERR("copy_to_user error (sdkey)");
				return -EFAULT;
			}
			break;

		default:
			PERR("func error\n");
			return -EFAULT;
		}
		break;

	default:
		PERR("command error\n");
		return -EFAULT;
	}
	return ret;
}


static int htc_sdservice_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int htc_sdservice_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static const struct file_operations htc_sdservice_fops = {
	.unlocked_ioctl = htc_sdservice_ioctl,
	.open = htc_sdservice_open,
	.release = htc_sdservice_release,
	.owner = THIS_MODULE,
};

static int __init htc_sdservice_init(void)
{
	int ret;

	htc_sdkey = kzalloc(HTC_SDKEY_LEN, GFP_KERNEL);
	if (htc_sdkey == NULL) {
		PERR("allocate the space for SD key failed\n");
		return -1;
	}

	ret = register_chrdev(0, DEVICE_NAME, &htc_sdservice_fops);
	if (ret < 0) {
		PERR("register module fail\n");
		return ret;
	}
	htc_sdservice_major = ret;

	htc_sdservice_class = class_create(THIS_MODULE, "htc_sdservice");
	device_create(htc_sdservice_class, NULL, MKDEV(htc_sdservice_major, 0), NULL, DEVICE_NAME);

	PDEBUG("register module ok\n");
	return 0;
}


static void  __exit htc_sdservice_exit(void)
{
	device_destroy(htc_sdservice_class, MKDEV(htc_sdservice_major, 0));
	class_unregister(htc_sdservice_class);
	class_destroy(htc_sdservice_class);
	unregister_chrdev(htc_sdservice_major, DEVICE_NAME);
	kfree(htc_sdkey);
	PDEBUG("un-registered module ok\n");
}

module_init(htc_sdservice_init);
module_exit(htc_sdservice_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tommy Chiu");

