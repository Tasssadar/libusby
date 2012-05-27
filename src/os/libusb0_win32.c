#include "libusb0_win32.h"
#include <windows.h>
#include <stdio.h>
#include <assert.h>

#include "libusb0_win32_intf.h"

typedef BOOL WINAPI cancel_io_ex_t(HANDLE hFile, LPOVERLAPPED lpOverlapped);

typedef struct libusb0_ctx
{
	HMODULE hKernel32;
	cancel_io_ex_t * cancel_io_ex;
} libusb0_ctx;

typedef struct libusb0_device_private
{
	int devno;
	HANDLE hFile;
} libusb0_device_private;

static int sync_device_io_control(HANDLE hFile, DWORD dwControlCode, void const * in_data, int in_len, void * out_data, int out_len)
{
	DWORD dwTransferred = 0;
	DWORD dwError = ERROR_SUCCESS;

	OVERLAPPED o = {0};
	o.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!o.hEvent)
		return LIBUSBY_ERROR_NO_MEM;

	if (!DeviceIoControl(hFile, dwControlCode, (void *)in_data, in_len, out_data, out_len, &dwTransferred, &o))
	{
		dwError = GetLastError();
		if (dwError == ERROR_IO_PENDING)
		{
			if (!GetOverlappedResult(hFile, &o, &dwTransferred, TRUE))
				dwError = GetLastError();
			else
				dwError = ERROR_SUCCESS;
		}
	}

	CloseHandle(o.hEvent);

	switch (dwError)
	{
	case ERROR_SUCCESS:
		return dwTransferred;
	case ERROR_BUSY:
		return LIBUSBY_ERROR_BUSY;
	default:
		return LIBUSBY_ERROR_IO;
	}
}

static int libusb0_init(libusby_context *ctx)
{
	libusb0_ctx * ctxpriv = usbyi_ctx_to_priv(ctx);

	ctxpriv->hKernel32 = LoadLibraryW(L"kernel32.dll");
	if (!ctxpriv->hKernel32)
		return LIBUSBY_ERROR_INVALID_PARAM;

	ctxpriv->cancel_io_ex = (cancel_io_ex_t *)GetProcAddress(ctxpriv->hKernel32, "CancelIoEx");
	return LIBUSBY_SUCCESS;
}

static void libusb0_exit(libusby_context *ctx)
{
	libusb0_ctx * ctxpriv = usbyi_ctx_to_priv(ctx);
	FreeLibrary(ctxpriv->hKernel32);
}

static int libusb0_get_device_descriptor(HANDLE hFile, uint8_t * desc)
{
	int r;

	libusb0_win32_request req = {0};
	req.descriptor.type = 1;

	r = sync_device_io_control(hFile, LIBUSB_IOCTL_GET_DESCRIPTOR, &req, sizeof req, desc, sizeof(libusby_device_descriptor));
	if (r != sizeof(libusby_device_descriptor))
		r = LIBUSBY_ERROR_IO;
	return r;
}

static int libusb0_get_device_list(libusby_context * ctx, libusby_device *** list)
{
	libusb0_ctx * ctxpriv = usbyi_ctx_to_priv(ctx);
	usbyi_device_list devlist = {0};
	int i;
	for (i = 1; i < LIBUSB_MAX_NUMBER_OF_DEVICES; ++i)
	{
		int j;
		HANDLE hFile;

		WCHAR devname[32];
		wsprintf(devname, L"\\\\.\\libusb0-%04d", i);

		hFile = CreateFileW(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
		if (hFile == INVALID_HANDLE_VALUE)
			continue;

		for (j = 0; j < ctx->devices.count; ++j)
		{
			libusb0_device_private * devpriv = usbyi_dev_to_devpriv(ctx->devices.list[j]);

			if (devpriv->devno == i)
			{
				if (usbyi_append_device_list(&devlist, ctx->devices.list[j]) < 0)
					goto error;
				libusby_ref_device(ctx->devices.list[j]);
				break;
			}
		}

		if (j == ctx->devices.count)
		{
			uint8_t cached_desc[sizeof(libusby_device_descriptor)];
			libusby_device * newdev = usbyi_alloc_device(ctx);

			if (!newdev)
				continue;

			if (libusb0_get_device_descriptor(hFile, cached_desc) < 0
				|| usbyi_sanitize_device_desc(&newdev->device_desc, cached_desc) < 0
				|| usbyi_append_device_list(&devlist, newdev) < 0)
			{
				libusby_unref_device(newdev);
				CloseHandle(hFile);
			}
			else
			{
				libusb0_device_private * devpriv = usbyi_dev_to_devpriv(newdev);
				devpriv->devno = i;
				devpriv->hFile = hFile;
			}
		}
	}

	*list = devlist.list;
	return devlist.count;

error:
	if (devlist.list)
		libusby_free_device_list(devlist.list, 1);
	return LIBUSBY_ERROR_NO_MEM;
}

static int libusb0_claim_interface(libusby_device_handle * dev_handle, int interface_number)
{
	libusb0_device_private * devpriv = usbyi_dev_to_devpriv(dev_handle->dev);

	libusb0_win32_request req = {0};
	req.intf.interface_number = interface_number;

	return sync_device_io_control(devpriv->hFile, LIBUSB_IOCTL_CLAIM_INTERFACE, &req, sizeof req, 0, 0);
}

static int libusb0_release_interface(libusby_device_handle * dev_handle, int interface_number)
{
	libusb0_device_private * devpriv = usbyi_dev_to_devpriv(dev_handle->dev);

	libusb0_win32_request req = {0};
	req.intf.interface_number = interface_number;

	return sync_device_io_control(devpriv->hFile, LIBUSB_IOCTL_RELEASE_INTERFACE, &req, sizeof req, 0, 0);
}

static void libusb0_update_finished_transfer(libusby_transfer * tran, DWORD dwError, DWORD dwTransferred)
{
	switch (dwError)
	{
	case ERROR_SUCCESS:
		tran->actual_length = dwTransferred;
		tran->status = LIBUSBY_TRANSFER_COMPLETED;
		break;

	case ERROR_OPERATION_ABORTED:
		tran->actual_length = 0;
		tran->status = LIBUSBY_TRANSFER_CANCELLED;
		break;

	default:
		tran->actual_length = 0;
		tran->status = LIBUSBY_TRANSFER_ERROR;
	}
}

static int libusb0_submit_transfer(libusby_transfer * tran)
{
	libusb0_ctx * ctx_priv = usbyi_ctx_to_priv(tran->dev_handle->dev->ctx);
	usbyi_transfer * trani = usbyi_tran_to_trani(tran);
	libusb0_device_private * devpriv = usbyi_dev_to_devpriv(tran->dev_handle->dev);

	DWORD dwControlCode = 0;
	DWORD dwTransferred;
	BOOL res;

	/* Note that we don't need to keep this around during the whole operation. It is copied
	 * by the kernel into a temporary buffer. */
	libusb0_win32_request req = {0};

	if (tran->type == LIBUSBY_TRANSFER_TYPE_BULK || tran->type == LIBUSBY_TRANSFER_TYPE_INTERRUPT)
	{
		req.endpoint.endpoint = tran->endpoint;
		if (tran->endpoint & 0x80)
			dwControlCode = LIBUSB_IOCTL_INTERRUPT_OR_BULK_READ;
		else
			dwControlCode = LIBUSB_IOCTL_INTERRUPT_OR_BULK_WRITE;
	}
	else
	{
		return LIBUSBY_ERROR_INVALID_PARAM;
	}

	res = DeviceIoControl(devpriv->hFile, dwControlCode, &req, sizeof req, tran->buffer, tran->length, &dwTransferred, &trani->os_priv.overlapped);

	/* We might have completed synchronously, we still have to reap asynchronously though. */
	if (!res && GetLastError() != ERROR_IO_PENDING)
		return LIBUSBY_ERROR_IO;

	ResetEvent(trani->os_priv.hCompletionEvent);
	usbyi_win32_add_transfer(trani);
	return LIBUSBY_SUCCESS;
}

static int libusb0_cancel_transfer(libusby_transfer * tran)
{
	if (tran->dev_handle)
	{
		usbyi_transfer * trani = usbyi_tran_to_trani(tran);
		libusb0_device_private * devpriv = usbyi_dev_to_devpriv(tran->dev_handle->dev);
		libusb0_ctx * ctxpriv = usbyi_ctx_to_priv(tran->dev_handle->dev->ctx);

		if (ctxpriv->cancel_io_ex)
		{
			ctxpriv->cancel_io_ex(devpriv->hFile, &trani->os_priv.overlapped);
		}
		else
		{
			libusb0_win32_request req = {0};
			req.endpoint.endpoint = tran->endpoint;
			sync_device_io_control(devpriv->hFile, LIBUSB_IOCTL_ABORT_ENDPOINT, &req, sizeof req, 0, 0);
		}
	}
	return LIBUSBY_SUCCESS;
}

static void libusb0_reap_transfer(usbyi_transfer * trani)
{
	libusb0_ctx * ctx_priv = usbyi_ctx_to_priv(trani->ctx);
	libusby_transfer * tran = usbyi_trani_to_tran(trani);
	usbyi_os_transfer * tranos = &trani->os_priv;
	libusb0_device_private * devpriv = usbyi_dev_to_devpriv(tran->dev_handle->dev);

	DWORD dwTransferred;
	BOOL res = GetOverlappedResult(devpriv->hFile, &trani->os_priv.overlapped, &dwTransferred, TRUE);

	libusb0_update_finished_transfer(tran,  res? ERROR_SUCCESS: GetLastError(), dwTransferred);
	usbyi_win32_remove_transfer(trani);
	if (tran->callback)
		tran->callback(tran);
	SetEvent(tranos->hCompletionEvent);
}

usbyi_backend const libusb0_win32_backend =
{
	sizeof(libusb0_ctx),
	sizeof(libusb0_device_private),
	0,
	&libusb0_init,
	&libusb0_exit,
	&libusb0_get_device_list,
	0,
	0,
	&libusb0_claim_interface,
	&libusb0_release_interface,
	0,
	&libusb0_submit_transfer,
	&libusb0_cancel_transfer,
	&libusb0_reap_transfer,
};