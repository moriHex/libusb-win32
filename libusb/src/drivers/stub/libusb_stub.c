/* LIBUSB-WIN32, Generic Windows USB Driver
 * Copyright (C) 2002-2003 Stephan Meyer, <ste_meyer@web.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */



#include <wdm.h>


typedef struct
{
  DEVICE_OBJECT	*self;
  DEVICE_OBJECT	*next_stack_device;
} libusb_device_extension;


void __stdcall unload(DRIVER_OBJECT *driver_object);
NTSTATUS __stdcall add_device(DRIVER_OBJECT *driver_object, 
			      DEVICE_OBJECT *physical_device_object);
static NTSTATUS dispatch(DEVICE_OBJECT *device_object, IRP *irp);
static NTSTATUS dispatch_pnp(libusb_device_extension *device_extension, 
			     IRP *irp);
static NTSTATUS complete_irp(libusb_device_extension *device_extension,
			     IRP *irp, NTSTATUS status, ULONG info);
static NTSTATUS on_start_completion(DEVICE_OBJECT *device_object, IRP *irp, 
				    void *event);

NTSTATUS __stdcall DriverEntry(DRIVER_OBJECT *driver_object,
			       UNICODE_STRING *registry_path)
{
  PDRIVER_DISPATCH *dispatch_function = driver_object->MajorFunction;
  int i;
  
  KdPrint(("LIBUSB_STUB - DriverEntry()\n"));
      
  for(i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++, dispatch_function++) 
    {
      *dispatch_function = dispatch;
    }
  
  driver_object->DriverExtension->AddDevice = add_device;
  driver_object->DriverUnload = unload;

  return STATUS_SUCCESS;
}

NTSTATUS __stdcall add_device(DRIVER_OBJECT *driver_object, 
			      DEVICE_OBJECT *physical_device_object)
{
  NTSTATUS status;
  DEVICE_OBJECT *device_object;
  libusb_device_extension *device_extension;
  int i, j;

  KdPrint(("LIBUSBSTUB - add_device(): creating device\n"));

  status = IoCreateDevice(driver_object, sizeof(libusb_device_extension), 
			  NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, 
			  &device_object);
  if(!NT_SUCCESS(status))
    {
      KdPrint(("LIBUSBSTUB - add_device(): creating device failed\n"));
      return status;
    }

  device_extension = (libusb_device_extension *)device_object->DeviceExtension;
  device_extension->self = device_object;
  device_extension->next_stack_device = 
    IoAttachDeviceToDeviceStack(device_object, physical_device_object);
  
  device_object->Flags |= DO_DIRECT_IO | DO_POWER_PAGABLE;
  device_object->Flags &= ~DO_DEVICE_INITIALIZING;

  return STATUS_SUCCESS;
}


NTSTATUS __stdcall dispatch(DEVICE_OBJECT *device_object, IRP *irp)
{
  NTSTATUS status = STATUS_SUCCESS;
  libusb_device_extension *device_extension = 
    (libusb_device_extension *)device_object->DeviceExtension;
  IO_STACK_LOCATION *stack_location = IoGetCurrentIrpStackLocation(irp);

  switch(stack_location->MajorFunction) 
    {
    case IRP_MJ_PNP:
      return dispatch_pnp(device_extension, irp);
      
    case IRP_MJ_POWER:
      PoStartNextPowerIrp(irp);
      IoSkipCurrentIrpStackLocation(irp);
      return PoCallDriver(device_extension->next_stack_device, irp);
      
    case IRP_MJ_DEVICE_CONTROL:
    case IRP_MJ_CREATE:      
    case IRP_MJ_CLOSE:
    default:
      ;
    }

  irp->IoStatus.Status = status;
  IoCompleteRequest(irp, IO_NO_INCREMENT);
 
  return status;
}

NTSTATUS dispatch_pnp(libusb_device_extension *device_extension, IRP *irp)
{
  NTSTATUS status = STATUS_SUCCESS;
  IO_STACK_LOCATION *stack_location = IoGetCurrentIrpStackLocation(irp);

  KEVENT event;

  switch(stack_location->MinorFunction) 
    {      
    case IRP_MN_START_DEVICE:

      KeInitializeEvent(&event, NotificationEvent, FALSE);
      
      IoCopyCurrentIrpStackLocationToNext(irp);
      IoSetCompletionRoutine(irp, 
			     (PIO_COMPLETION_ROUTINE)on_start_completion, 
			     (void*)&event, TRUE, TRUE, TRUE);
      
      status = IoCallDriver(device_extension->next_stack_device, irp);
      
      if(status == STATUS_PENDING) 
	{
	  KeWaitForSingleObject(&event, Executive, 
				KernelMode, FALSE, NULL);
	  status = irp->IoStatus.Status;
	}

      if(!(NT_SUCCESS(status) && NT_SUCCESS(irp->IoStatus.Status)))
	{ 
	  KdPrint(("LIBUSB_STUB - dispatch_pnp(): calling lower driver failed\n"));
	  return status;
	}

      irp->IoStatus.Status = status;
      IoCompleteRequest(irp, IO_NO_INCREMENT);
      break;

    case IRP_MN_REMOVE_DEVICE:

      IoSkipCurrentIrpStackLocation(irp);
      status = IoCallDriver(device_extension->next_stack_device, irp);


      if(!NT_SUCCESS(status))
	{ 
	  KdPrint(("LIBUSB_STUB - dispatch_pnp(): calling lower driver "
		   "failed\n"));
	  return status;
	}

      IoDetachDevice(device_extension->next_stack_device);
      IoDeleteDevice(device_extension->self);

      return status;

    case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
    default:
      IoSkipCurrentIrpStackLocation (irp);
      status = IoCallDriver (device_extension->next_stack_device, irp);
    }

  return status;
}


VOID __stdcall unload(DRIVER_OBJECT *driver_object)
{
  KdPrint(("LIBUSB_STUB - dispatch_pnp(): unloading driver\n"));
}


NTSTATUS on_start_completion(DEVICE_OBJECT *device_object, 
			     IRP *irp, void *event)
{
  KeSetEvent((KEVENT *)event, IO_NO_INCREMENT, FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}
