/*
 * parallels_sdk.c: core driver functions for managing
 * Parallels Cloud Server hosts
 *
 * Copyright (C) 2014 Parallels, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>

#include "virerror.h"
#include "viralloc.h"
#include "virstring.h"
#include "nodeinfo.h"
#include "virlog.h"
#include "datatypes.h"
#include "domain_conf.h"

#include "parallels_sdk.h"

#define VIR_FROM_THIS VIR_FROM_PARALLELS
#define JOB_INFINIT_WAIT_TIMEOUT UINT_MAX

VIR_LOG_INIT("parallels.sdk");

/*
 * Log error description
 */
static void
logPrlErrorHelper(PRL_RESULT err, const char *filename,
                  const char *funcname, size_t linenr)
{
    char *msg1 = NULL, *msg2 = NULL;
    PRL_UINT32 len = 0;

    /* Get required buffer length */
    PrlApi_GetResultDescription(err, PRL_TRUE, PRL_FALSE, NULL, &len);

    if (VIR_ALLOC_N(msg1, len) < 0)
        goto cleanup;

    /* get short error description */
    PrlApi_GetResultDescription(err, PRL_TRUE, PRL_FALSE, msg1, &len);

    PrlApi_GetResultDescription(err, PRL_FALSE, PRL_FALSE, NULL, &len);

    if (VIR_ALLOC_N(msg2, len) < 0)
        goto cleanup;

    /* get long error description */
    PrlApi_GetResultDescription(err, PRL_FALSE, PRL_FALSE, msg2, &len);

    virReportErrorHelper(VIR_FROM_THIS, VIR_ERR_INTERNAL_ERROR,
                         filename, funcname, linenr,
                         _("%s %s"), msg1, msg2);

 cleanup:
    VIR_FREE(msg1);
    VIR_FREE(msg2);
}

#define logPrlError(code)                          \
    logPrlErrorHelper(code, __FILE__,              \
                         __FUNCTION__, __LINE__)

#define prlsdkCheckRetGoto(ret, label)             \
    do {                                           \
        if (PRL_FAILED(ret)) {                     \
            logPrlError(ret);                      \
            goto label;                            \
        }                                          \
    } while (0)

static PRL_RESULT
logPrlEventErrorHelper(PRL_HANDLE event, const char *filename,
                       const char *funcname, size_t linenr)
{
    PRL_RESULT ret, retCode;
    char *msg1 = NULL, *msg2 = NULL;
    PRL_UINT32 len = 0;
    int err = -1;

    if ((ret = PrlEvent_GetErrCode(event, &retCode))) {
        logPrlError(ret);
        return ret;
    }

    PrlEvent_GetErrString(event, PRL_TRUE, PRL_FALSE, NULL, &len);

    if (VIR_ALLOC_N(msg1, len) < 0)
        goto cleanup;

    PrlEvent_GetErrString(event, PRL_TRUE, PRL_FALSE, msg1, &len);

    PrlEvent_GetErrString(event, PRL_FALSE, PRL_FALSE, NULL, &len);

    if (VIR_ALLOC_N(msg2, len) < 0)
        goto cleanup;

    PrlEvent_GetErrString(event, PRL_FALSE, PRL_FALSE, msg2, &len);

    virReportErrorHelper(VIR_FROM_THIS, VIR_ERR_INTERNAL_ERROR,
                         filename, funcname, linenr,
                         _("%s %s"), msg1, msg2);
    err = 0;

 cleanup:
    VIR_FREE(msg1);
    VIR_FREE(msg2);

    return err;
}

#define logPrlEventError(event)                    \
    logPrlEventErrorHelper(event, __FILE__,        \
                         __FUNCTION__, __LINE__)

static PRL_RESULT
getJobResultHelper(PRL_HANDLE job, unsigned int timeout, PRL_HANDLE *result,
                   const char *filename, const char *funcname,
                   size_t linenr)
{
    PRL_RESULT ret, retCode;

    if ((ret = PrlJob_Wait(job, timeout))) {
        logPrlErrorHelper(ret, filename, funcname, linenr);
        goto cleanup;
    }

    if ((ret = PrlJob_GetRetCode(job, &retCode))) {
        logPrlErrorHelper(ret, filename, funcname, linenr);
        goto cleanup;
    }

    if (retCode) {
        PRL_HANDLE err_handle;

        /* Sometimes it's possible to get additional error info. */
        if ((ret = PrlJob_GetError(job, &err_handle))) {
            logPrlErrorHelper(ret, filename, funcname, linenr);
            goto cleanup;
        }

        if (logPrlEventErrorHelper(err_handle, filename, funcname, linenr))
            logPrlErrorHelper(retCode, filename, funcname, linenr);

        PrlHandle_Free(err_handle);
        ret = retCode;
    } else {
        ret = PrlJob_GetResult(job, result);
        if (PRL_FAILED(ret)) {
            logPrlErrorHelper(ret, filename, funcname, linenr);
            PrlHandle_Free(*result);
            *result = NULL;
            goto cleanup;
        }

        ret = PRL_ERR_SUCCESS;
    }

 cleanup:
    PrlHandle_Free(job);
    return ret;
}

#define getJobResult(job, result)                       \
    getJobResultHelper(job, JOB_INFINIT_WAIT_TIMEOUT,   \
            result, __FILE__, __FUNCTION__, __LINE__)

static PRL_RESULT
waitJobHelper(PRL_HANDLE job, unsigned int timeout,
              const char *filename, const char *funcname,
              size_t linenr)
{
    PRL_HANDLE result = PRL_INVALID_HANDLE;
    PRL_RESULT ret;

    ret = getJobResultHelper(job, timeout, &result,
                             filename, funcname, linenr);
    PrlHandle_Free(result);
    return ret;
}

#define waitJob(job)                                        \
    waitJobHelper(job, JOB_INFINIT_WAIT_TIMEOUT, __FILE__,  \
                         __FUNCTION__, __LINE__)


int
prlsdkInit(void)
{
    PRL_RESULT ret;

    ret = PrlApi_InitEx(PARALLELS_API_VER, PAM_SERVER, 0, 0);
    if (PRL_FAILED(ret)) {
        logPrlError(ret);
        return -1;
    }

    return 0;
};

void
prlsdkDeinit(void)
{
    PrlApi_Deinit();
};

int
prlsdkConnect(parallelsConnPtr privconn)
{
    PRL_RESULT ret;
    PRL_HANDLE job = PRL_INVALID_HANDLE;

    ret = PrlSrv_Create(&privconn->server);
    if (PRL_FAILED(ret)) {
        logPrlError(ret);
        return -1;
    }

    job = PrlSrv_LoginLocalEx(privconn->server, NULL, 0,
                              PSL_HIGH_SECURITY, PACF_NON_INTERACTIVE_MODE);

    if (waitJob(job)) {
        PrlHandle_Free(privconn->server);
        return -1;
    }

    return 0;
}

void
prlsdkDisconnect(parallelsConnPtr privconn)
{
    PRL_HANDLE job;

    job = PrlSrv_Logoff(privconn->server);
    waitJob(job);

    PrlHandle_Free(privconn->server);
}

static int
prlsdkSdkDomainLookup(parallelsConnPtr privconn,
                      const char *id,
                      unsigned int flags,
                      PRL_HANDLE *sdkdom)
{
    PRL_HANDLE job = PRL_INVALID_HANDLE;
    PRL_HANDLE result = PRL_INVALID_HANDLE;
    PRL_RESULT pret = PRL_ERR_UNINITIALIZED;
    int ret = -1;

    job = PrlSrv_GetVmConfig(privconn->server, id, flags);
    if (PRL_FAILED(getJobResult(job, &result)))
        goto cleanup;

    pret = PrlResult_GetParamByIndex(result, 0, sdkdom);
    prlsdkCheckRetGoto(pret, cleanup);

    ret = 0;

 cleanup:
    PrlHandle_Free(result);
    return ret;
}

static void
prlsdkUUIDFormat(const unsigned char *uuid, char *uuidstr)
{
    virUUIDFormat(uuid, uuidstr + 1);

    uuidstr[0] = '{';
    uuidstr[VIR_UUID_STRING_BUFLEN] = '}';
    uuidstr[VIR_UUID_STRING_BUFLEN + 1] = '\0';
}

static PRL_HANDLE
prlsdkSdkDomainLookupByUUID(parallelsConnPtr privconn, const unsigned char *uuid)
{
    char uuidstr[VIR_UUID_STRING_BUFLEN + 2];
    PRL_HANDLE sdkdom = PRL_INVALID_HANDLE;

    prlsdkUUIDFormat(uuid, uuidstr);

    if (prlsdkSdkDomainLookup(privconn, uuidstr,
                              PGVC_SEARCH_BY_UUID, &sdkdom) < 0) {
        virUUIDFormat(uuid, uuidstr);
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching uuid '%s'"), uuidstr);
        return PRL_INVALID_HANDLE;
    }

    return sdkdom;
}

static int
prlsdkUUIDParse(const char *uuidstr, unsigned char *uuid)
{
    char *tmp = NULL;
    int ret = -1;

    virCheckNonNullArgGoto(uuidstr, error);
    virCheckNonNullArgGoto(uuid, error);

    if (VIR_STRDUP(tmp, uuidstr) < 0)
        goto error;

    tmp[strlen(tmp) - 1] = '\0';

    /* trim curly braces */
    if (virUUIDParse(tmp + 1, uuid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("UUID in config file malformed"));
        ret = -1;
        goto error;
    }

    ret = 0;
 error:
    VIR_FREE(tmp);
    return ret;
}

static int
prlsdkGetDomainIds(PRL_HANDLE sdkdom,
                   char **name,
                   unsigned char *uuid)
{
    char uuidstr[VIR_UUID_STRING_BUFLEN + 2];
    PRL_UINT32 len;
    PRL_RESULT pret;

    len = 0;
    /* get name length */
    pret = PrlVmCfg_GetName(sdkdom, NULL, &len);
    prlsdkCheckRetGoto(pret, error);

    if (VIR_ALLOC_N(*name, len) < 0)
        goto error;

    PrlVmCfg_GetName(sdkdom, *name, &len);
    prlsdkCheckRetGoto(pret, error);

    len = sizeof(uuidstr);
    PrlVmCfg_GetUuid(sdkdom, uuidstr, &len);
    prlsdkCheckRetGoto(pret, error);

    if (prlsdkUUIDParse(uuidstr, uuid) < 0)
        goto error;

    return 0;

 error:
    VIR_FREE(*name);
    return -1;
}

static int
prlsdkGetDomainState(PRL_HANDLE sdkdom, VIRTUAL_MACHINE_STATE_PTR vmState)
{
    PRL_HANDLE job = PRL_INVALID_HANDLE;
    PRL_HANDLE result = PRL_INVALID_HANDLE;
    PRL_HANDLE vmInfo = PRL_INVALID_HANDLE;
    PRL_RESULT pret;
    int ret = -1;

    job = PrlVm_GetState(sdkdom);

    if (PRL_FAILED(getJobResult(job, &result)))
        goto cleanup;

    pret = PrlResult_GetParamByIndex(result, 0, &vmInfo);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmInfo_GetState(vmInfo, vmState);
    prlsdkCheckRetGoto(pret, cleanup);

    ret = 0;

 cleanup:
    PrlHandle_Free(vmInfo);
    PrlHandle_Free(result);
    return ret;
}

static void
prlsdkDomObjFreePrivate(void *p)
{
    parallelsDomObjPtr pdom = p;

    if (!pdom)
        return;

    PrlHandle_Free(pdom->sdkdom);
    VIR_FREE(pdom->uuid);
    VIR_FREE(pdom->home);
    VIR_FREE(p);
};

static int
prlsdkAddDomainVideoInfo(PRL_HANDLE sdkdom, virDomainDefPtr def)
{
    virDomainVideoDefPtr video = NULL;
    virDomainVideoAccelDefPtr accel = NULL;
    PRL_RESULT ret;
    PRL_UINT32 videoRam;

    /* video info */
    ret = PrlVmCfg_GetVideoRamSize(sdkdom, &videoRam);
    prlsdkCheckRetGoto(ret, error);

    if (VIR_ALLOC(video) < 0)
        goto error;

    if (VIR_ALLOC(accel) < 0)
        goto error;

    if (VIR_APPEND_ELEMENT_COPY(def->videos, def->nvideos, video) < 0)
        goto error;

    video->type = VIR_DOMAIN_VIDEO_TYPE_VGA;
    video->vram = videoRam << 10; /* from mbibytes to kbibytes */
    video->heads = 1;
    video->accel = accel;

    return 0;

 error:
    VIR_FREE(accel);
    virDomainVideoDefFree(video);
    return -1;
}

static int
prlsdkGetDiskInfo(PRL_HANDLE prldisk,
                  virDomainDiskDefPtr disk,
                  bool isCdrom)
{
    char *buf = NULL;
    PRL_UINT32 buflen = 0;
    PRL_RESULT pret;
    PRL_UINT32 emulatedType;
    PRL_UINT32 ifType;
    PRL_UINT32 pos;
    virDomainDeviceDriveAddressPtr address;
    int ret = -1;

    pret = PrlVmDev_GetEmulatedType(prldisk, &emulatedType);
    prlsdkCheckRetGoto(pret, cleanup);
    if (emulatedType == PDT_USE_IMAGE_FILE) {
        virDomainDiskSetType(disk, VIR_STORAGE_TYPE_FILE);
        if (isCdrom)
            virDomainDiskSetFormat(disk, VIR_STORAGE_FILE_RAW);
        else
            virDomainDiskSetFormat(disk, VIR_STORAGE_FILE_PLOOP);
    } else {
        virDomainDiskSetType(disk, VIR_STORAGE_TYPE_BLOCK);
        virDomainDiskSetFormat(disk, VIR_STORAGE_FILE_RAW);
    }

    if (isCdrom) {
        disk->device = VIR_DOMAIN_DISK_DEVICE_CDROM;
        disk->src->readonly = true;
    } else {
        disk->device = VIR_DOMAIN_DISK_DEVICE_DISK;
    }

    pret = PrlVmDev_GetFriendlyName(prldisk, NULL, &buflen);
    prlsdkCheckRetGoto(pret, cleanup);

    if (VIR_ALLOC_N(buf, buflen) < 0)
        goto cleanup;

    pret = PrlVmDev_GetFriendlyName(prldisk, buf, &buflen);
    prlsdkCheckRetGoto(pret, cleanup);

    if (virDomainDiskSetSource(disk, buf) < 0)
        goto cleanup;

    pret = PrlVmDev_GetIfaceType(prldisk, &ifType);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_GetStackIndex(prldisk, &pos);
    prlsdkCheckRetGoto(pret, cleanup);

    address = &disk->info.addr.drive;
    switch (ifType) {
    case PMS_IDE_DEVICE:
        disk->bus = VIR_DOMAIN_DISK_BUS_IDE;
        disk->dst = virIndexToDiskName(pos, "hd");
        address->bus = pos / 2;
        address->target = 0;
        address->unit = pos % 2;
        break;
    case PMS_SCSI_DEVICE:
        disk->bus = VIR_DOMAIN_DISK_BUS_SCSI;
        disk->dst = virIndexToDiskName(pos, "sd");
        address->bus = 0;
        address->target = 0;
        address->unit = pos;
        break;
    case PMS_SATA_DEVICE:
        disk->bus = VIR_DOMAIN_DISK_BUS_SATA;
        disk->dst = virIndexToDiskName(pos, "sd");
        address->bus = 0;
        address->target = 0;
        address->unit = pos;
        break;
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unknown disk bus: %X"), ifType);
        goto cleanup;
        break;
    }

    if (!disk->dst)
        goto cleanup;

    disk->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE;

    ret = 0;

 cleanup:
    VIR_FREE(buf);
    return ret;
}

static int
prlsdkGetFSInfo(PRL_HANDLE prldisk,
                virDomainFSDefPtr fs)
{
    char *buf = NULL;
    PRL_UINT32 buflen = 0;
    PRL_RESULT pret;
    int ret = -1;

    fs->type = VIR_DOMAIN_FS_TYPE_FILE;
    fs->fsdriver = VIR_DOMAIN_FS_DRIVER_TYPE_PLOOP;
    fs->accessmode = VIR_DOMAIN_FS_ACCESSMODE_PASSTHROUGH;
    fs->wrpolicy = VIR_DOMAIN_FS_WRPOLICY_DEFAULT;
    fs->format = VIR_STORAGE_FILE_PLOOP;

    fs->readonly = false;
    fs->symlinksResolved = false;

    pret = PrlVmDev_GetImagePath(prldisk, NULL, &buflen);
    prlsdkCheckRetGoto(pret, cleanup);

    if (VIR_ALLOC_N(buf, buflen) < 0)
        goto cleanup;

    pret = PrlVmDev_GetImagePath(prldisk, buf, &buflen);
    prlsdkCheckRetGoto(pret, cleanup);

    fs->src = buf;
    buf = NULL;

    pret = PrlVmDevHd_GetMountPoint(prldisk, NULL, &buflen);
    prlsdkCheckRetGoto(pret, cleanup);

    if (VIR_ALLOC_N(buf, buflen) < 0)
        goto cleanup;

    pret = PrlVmDevHd_GetMountPoint(prldisk, buf, &buflen);
    prlsdkCheckRetGoto(pret, cleanup);

    fs->dst = buf;
    buf = NULL;

    ret = 0;

 cleanup:
    VIR_FREE(buf);
    return ret;
}

static int
prlsdkAddDomainHardDisksInfo(PRL_HANDLE sdkdom, virDomainDefPtr def)
{
    PRL_RESULT pret;
    PRL_UINT32 hddCount;
    PRL_UINT32 i;
    PRL_HANDLE hdd = PRL_INVALID_HANDLE;
    virDomainDiskDefPtr disk = NULL;
    virDomainFSDefPtr fs = NULL;

    pret = PrlVmCfg_GetHardDisksCount(sdkdom, &hddCount);
    prlsdkCheckRetGoto(pret, error);

    for (i = 0; i < hddCount; ++i) {
        pret = PrlVmCfg_GetHardDisk(sdkdom, i, &hdd);
        prlsdkCheckRetGoto(pret, error);

        if (IS_CT(def)) {

            if (VIR_ALLOC(fs) < 0)
                goto error;

            if (prlsdkGetFSInfo(hdd, fs) < 0)
                goto error;

            if (virDomainFSInsert(def, fs) < 0)
                goto error;

            fs = NULL;
            PrlHandle_Free(hdd);
            hdd = PRL_INVALID_HANDLE;
        } else {
            if (!(disk = virDomainDiskDefNew(NULL)))
                goto error;

            if (prlsdkGetDiskInfo(hdd, disk, false) < 0)
                goto error;

            if (VIR_APPEND_ELEMENT(def->disks, def->ndisks, disk) < 0)
                goto error;

            disk = NULL;
            PrlHandle_Free(hdd);
            hdd = PRL_INVALID_HANDLE;
        }
    }

    return 0;

 error:
    PrlHandle_Free(hdd);
    virDomainDiskDefFree(disk);
    virDomainFSDefFree(fs);
    return -1;
}

static int
prlsdkAddDomainOpticalDisksInfo(PRL_HANDLE sdkdom, virDomainDefPtr def)
{
    PRL_RESULT pret;
    PRL_UINT32 cdromsCount;
    PRL_UINT32 i;
    PRL_HANDLE cdrom = PRL_INVALID_HANDLE;
    virDomainDiskDefPtr disk = NULL;

    pret = PrlVmCfg_GetOpticalDisksCount(sdkdom, &cdromsCount);
    prlsdkCheckRetGoto(pret, error);

    for (i = 0; i < cdromsCount; ++i) {
        pret = PrlVmCfg_GetOpticalDisk(sdkdom, i, &cdrom);
        prlsdkCheckRetGoto(pret, error);

        if (!(disk = virDomainDiskDefNew(NULL)))
            goto error;

        if (prlsdkGetDiskInfo(cdrom, disk, true) < 0)
            goto error;

        PrlHandle_Free(cdrom);
        cdrom = PRL_INVALID_HANDLE;

        if (VIR_APPEND_ELEMENT(def->disks, def->ndisks, disk) < 0)
            goto error;
    }

    return 0;

 error:
    PrlHandle_Free(cdrom);
    virDomainDiskDefFree(disk);
    return -1;
}

static int
prlsdkGetNetInfo(PRL_HANDLE netAdapter, virDomainNetDefPtr net, bool isCt)
{
    char macstr[VIR_MAC_STRING_BUFLEN];
    PRL_UINT32 buflen;
    PRL_UINT32 netAdapterIndex;
    PRL_UINT32 emulatedType;
    PRL_RESULT pret;
    PRL_BOOL isConnected;
    int ret = -1;

    net->type = VIR_DOMAIN_NET_TYPE_NETWORK;


    /* use device name, shown by prlctl as target device
     * for identifying network adapter in virDomainDefineXML */
    pret = PrlVmDevNet_GetHostInterfaceName(netAdapter, NULL, &buflen);
    prlsdkCheckRetGoto(pret, cleanup);

    if (VIR_ALLOC_N(net->ifname, buflen) < 0)
        goto cleanup;

    pret = PrlVmDevNet_GetHostInterfaceName(netAdapter, net->ifname, &buflen);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_GetIndex(netAdapter, &netAdapterIndex);
    prlsdkCheckRetGoto(pret, cleanup);

    if (isCt && netAdapterIndex == (PRL_UINT32) -1) {
        /* venet devices don't have mac address and
         * always up */
        net->linkstate = VIR_DOMAIN_NET_INTERFACE_LINK_STATE_UP;
        if (VIR_STRDUP(net->data.network.name,
                       PARALLELS_DOMAIN_ROUTED_NETWORK_NAME) < 0)
            goto cleanup;
        return 0;
    }

    buflen = ARRAY_CARDINALITY(macstr);
    if (VIR_ALLOC_N(macstr, buflen))
        goto cleanup;
    pret = PrlVmDevNet_GetMacAddressCanonical(netAdapter, macstr, &buflen);
    prlsdkCheckRetGoto(pret, cleanup);

    if (virMacAddrParse(macstr, &net->mac) < 0)
        goto cleanup;

    pret = PrlVmDev_GetEmulatedType(netAdapter, &emulatedType);
    prlsdkCheckRetGoto(pret, cleanup);

    if (emulatedType == PNA_ROUTED) {
        if (VIR_STRDUP(net->data.network.name,
                       PARALLELS_DOMAIN_ROUTED_NETWORK_NAME) < 0)
            goto cleanup;
    } else {
        pret = PrlVmDevNet_GetVirtualNetworkId(netAdapter, NULL, &buflen);
        prlsdkCheckRetGoto(pret, cleanup);

        if (VIR_ALLOC_N(net->data.network.name, buflen) < 0)
            goto cleanup;

        pret = PrlVmDevNet_GetVirtualNetworkId(netAdapter,
                                               net->data.network.name,
                                               &buflen);
        prlsdkCheckRetGoto(pret, cleanup);

        /*
         * We use VIR_DOMAIN_NET_TYPE_NETWORK for all network adapters
         * except those whose Virtual Network Id differ from Parallels
         * predefined ones such as PARALLELS_DOMAIN_BRIDGED_NETWORK_NAME
         * and PARALLELS_DONAIN_ROUTED_NETWORK_NAME
         */
        if (STRNEQ(net->data.network.name, PARALLELS_DOMAIN_BRIDGED_NETWORK_NAME))
            net->type = VIR_DOMAIN_NET_TYPE_BRIDGE;

    }

    if (!isCt) {
        PRL_VM_NET_ADAPTER_TYPE type;
        pret = PrlVmDevNet_GetAdapterType(netAdapter, &type);
        prlsdkCheckRetGoto(pret, cleanup);

        switch (type) {
        case PNT_RTL:
            if (VIR_STRDUP(net->model, "rtl8139") < 0)
                goto cleanup;
            break;
        case PNT_E1000:
            if (VIR_STRDUP(net->model, "e1000") < 0)
                goto cleanup;
            break;
        case PNT_VIRTIO:
            if (VIR_STRDUP(net->model, "virtio") < 0)
                goto cleanup;
            break;
        default:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown adapter type: %X"), type);
            goto cleanup;
        }
    }

    pret = PrlVmDev_IsConnected(netAdapter, &isConnected);
    prlsdkCheckRetGoto(pret, cleanup);

    if (isConnected)
        net->linkstate = VIR_DOMAIN_NET_INTERFACE_LINK_STATE_UP;
    else
        net->linkstate = VIR_DOMAIN_NET_INTERFACE_LINK_STATE_DOWN;

    ret = 0;
 cleanup:
    return ret;
}

static int
prlsdkAddDomainNetInfo(PRL_HANDLE sdkdom, virDomainDefPtr def)
{
    virDomainNetDefPtr net = NULL;
    PRL_RESULT ret;
    PRL_HANDLE netAdapter;
    PRL_UINT32 netAdaptersCount;
    PRL_UINT32 i;

    ret = PrlVmCfg_GetNetAdaptersCount(sdkdom, &netAdaptersCount);
    prlsdkCheckRetGoto(ret, error);
    for (i = 0; i < netAdaptersCount; ++i) {
        ret = PrlVmCfg_GetNetAdapter(sdkdom, i, &netAdapter);
        prlsdkCheckRetGoto(ret, error);

        if (VIR_ALLOC(net) < 0)
            goto error;

        if (prlsdkGetNetInfo(netAdapter, net, IS_CT(def)) < 0)
            goto error;

        PrlHandle_Free(netAdapter);
        netAdapter = PRL_INVALID_HANDLE;

        if (VIR_APPEND_ELEMENT(def->nets, def->nnets, net) < 0)
            goto error;
    }

    return 0;

 error:
    PrlHandle_Free(netAdapter);
    virDomainNetDefFree(net);
    return -1;
}

static int
prlsdkGetSerialInfo(PRL_HANDLE serialPort, virDomainChrDefPtr chr)
{
    PRL_RESULT pret;
    PRL_UINT32 serialPortIndex;
    PRL_UINT32 emulatedType;
    char *friendlyName = NULL;
    PRL_UINT32 buflen;

    chr->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL;
    chr->targetTypeAttr = false;
    pret = PrlVmDev_GetIndex(serialPort, &serialPortIndex);
    prlsdkCheckRetGoto(pret, error);
    chr->target.port = serialPortIndex;

    pret = PrlVmDev_GetEmulatedType(serialPort, &emulatedType);
    prlsdkCheckRetGoto(pret, error);

    pret = PrlVmDev_GetFriendlyName(serialPort, NULL, &buflen);
    prlsdkCheckRetGoto(pret, error);

    if (VIR_ALLOC_N(friendlyName, buflen) < 0)
        goto error;

    pret = PrlVmDev_GetFriendlyName(serialPort, friendlyName, &buflen);
    prlsdkCheckRetGoto(pret, error);

    switch (emulatedType) {
    case PDT_USE_OUTPUT_FILE:
        chr->source.type = VIR_DOMAIN_CHR_TYPE_FILE;
        chr->source.data.file.path = friendlyName;
        break;
    case PDT_USE_SERIAL_PORT_SOCKET_MODE:
        chr->source.type = VIR_DOMAIN_CHR_TYPE_UNIX;
        chr->source.data.nix.path = friendlyName;
        break;
    case PDT_USE_REAL_DEVICE:
        chr->source.type = VIR_DOMAIN_CHR_TYPE_DEV;
        chr->source.data.file.path = friendlyName;
        break;
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unknown serial type: %X"), emulatedType);
        goto error;
        break;
    }

    return 0;
 error:
    VIR_FREE(friendlyName);
    return -1;
}


static int
prlsdkAddSerialInfo(PRL_HANDLE sdkdom,
                    virDomainChrDefPtr **serials,
                    size_t *nserials)
{
    PRL_RESULT ret;
    PRL_HANDLE serialPort;
    PRL_UINT32 serialPortsCount;
    PRL_UINT32 i;
    virDomainChrDefPtr chr = NULL;

    ret = PrlVmCfg_GetSerialPortsCount(sdkdom, &serialPortsCount);
    prlsdkCheckRetGoto(ret, cleanup);
    for (i = 0; i < serialPortsCount; ++i) {
        ret = PrlVmCfg_GetSerialPort(sdkdom, i, &serialPort);
        prlsdkCheckRetGoto(ret, cleanup);

        if (!(chr = virDomainChrDefNew()))
            goto cleanup;

        if (prlsdkGetSerialInfo(serialPort, chr))
            goto cleanup;

        PrlHandle_Free(serialPort);
        serialPort = PRL_INVALID_HANDLE;

        if (VIR_APPEND_ELEMENT(*serials, *nserials, chr) < 0)
            goto cleanup;
    }

    return 0;

 cleanup:
    PrlHandle_Free(serialPort);
    virDomainChrDefFree(chr);
    return -1;
}


static int
prlsdkAddDomainHardware(PRL_HANDLE sdkdom, virDomainDefPtr def)
{
    if (!IS_CT(def))
        if (prlsdkAddDomainVideoInfo(sdkdom, def) < 0)
            goto error;

    if (prlsdkAddDomainHardDisksInfo(sdkdom, def) < 0)
        goto error;

    if (prlsdkAddDomainOpticalDisksInfo(sdkdom, def) < 0)
        goto error;

    if (prlsdkAddDomainNetInfo(sdkdom, def) < 0)
        goto error;

    if (prlsdkAddSerialInfo(sdkdom,
                            &def->serials,
                            &def->nserials) < 0)
        goto error;

    return 0;
 error:
    return -1;
}


static int
prlsdkAddVNCInfo(PRL_HANDLE sdkdom, virDomainDefPtr def)
{
    virDomainGraphicsDefPtr gr = NULL;
    PRL_VM_REMOTE_DISPLAY_MODE vncMode;
    PRL_UINT32 port;
    PRL_UINT32 buflen = 0;
    PRL_RESULT pret;

    pret = PrlVmCfg_GetVNCMode(sdkdom, &vncMode);
    prlsdkCheckRetGoto(pret, error);

    if (vncMode == PRD_DISABLED)
        return 0;

    if (VIR_ALLOC(gr) < 0)
        goto error;

    pret = PrlVmCfg_GetVNCPort(sdkdom, &port);
    prlsdkCheckRetGoto(pret, error);

    gr->data.vnc.autoport = (vncMode == PRD_AUTO);
    gr->type = VIR_DOMAIN_GRAPHICS_TYPE_VNC;
    gr->data.vnc.port = port;
    gr->data.vnc.keymap = NULL;
    gr->data.vnc.socket = NULL;
    gr->data.vnc.auth.passwd = NULL;
    gr->data.vnc.auth.expires = false;
    gr->data.vnc.auth.connected = 0;

    if (VIR_ALLOC(gr->listens) < 0)
        goto error;

    gr->nListens = 1;

    pret = PrlVmCfg_GetVNCHostName(sdkdom, NULL, &buflen);
    prlsdkCheckRetGoto(pret, error);

    if (VIR_ALLOC_N(gr->listens[0].address, buflen) < 0)
        goto error;

    pret = PrlVmCfg_GetVNCHostName(sdkdom, gr->listens[0].address, &buflen);
    prlsdkCheckRetGoto(pret, error);

    gr->listens[0].type = VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS;

    if (VIR_APPEND_ELEMENT(def->graphics, def->ngraphics, gr) < 0)
        goto error;

    if (IS_CT(def)) {
        virDomainVideoDefPtr video;
        if (VIR_ALLOC(video) < 0)
            goto error;
        video->type = virDomainVideoDefaultType(def);
        if (video->type < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("cannot determine default video type"));
            VIR_FREE(video);
            goto error;
        }
        video->vram = virDomainVideoDefaultRAM(def, video->type);
        video->heads = 1;
        if (VIR_ALLOC_N(def->videos, 1) < 0) {
            virDomainVideoDefFree(video);
            goto error;
        }
        def->videos[def->nvideos++] = video;
    }
    return 0;

 error:
    virDomainGraphicsDefFree(gr);
    return -1;
}

static int
prlsdkConvertDomainState(VIRTUAL_MACHINE_STATE domainState,
                         PRL_UINT32 envId,
                         virDomainObjPtr dom)
{
    switch (domainState) {
    case VMS_STOPPED:
    case VMS_MOUNTED:
        virDomainObjSetState(dom, VIR_DOMAIN_SHUTOFF,
                             VIR_DOMAIN_SHUTOFF_SHUTDOWN);
        dom->def->id = -1;
        break;
    case VMS_STARTING:
    case VMS_COMPACTING:
    case VMS_RESETTING:
    case VMS_PAUSING:
    case VMS_RECONNECTING:
    case VMS_RUNNING:
        virDomainObjSetState(dom, VIR_DOMAIN_RUNNING,
                             VIR_DOMAIN_RUNNING_BOOTED);
        dom->def->id = envId;
        break;
    case VMS_PAUSED:
        virDomainObjSetState(dom, VIR_DOMAIN_PAUSED,
                             VIR_DOMAIN_PAUSED_USER);
        dom->def->id = envId;
        break;
    case VMS_SUSPENDED:
    case VMS_DELETING_STATE:
    case VMS_SUSPENDING_SYNC:
        virDomainObjSetState(dom, VIR_DOMAIN_SHUTOFF,
                             VIR_DOMAIN_SHUTOFF_SAVED);
        dom->def->id = -1;
        break;
    case VMS_STOPPING:
        virDomainObjSetState(dom, VIR_DOMAIN_SHUTDOWN,
                             VIR_DOMAIN_SHUTDOWN_USER);
        dom->def->id = envId;
        break;
    case VMS_SNAPSHOTING:
        virDomainObjSetState(dom, VIR_DOMAIN_PAUSED,
                             VIR_DOMAIN_PAUSED_SNAPSHOT);
        dom->def->id = envId;
        break;
    case VMS_MIGRATING:
        virDomainObjSetState(dom, VIR_DOMAIN_PAUSED,
                             VIR_DOMAIN_PAUSED_MIGRATION);
        dom->def->id = envId;
        break;
    case VMS_SUSPENDING:
        virDomainObjSetState(dom, VIR_DOMAIN_PAUSED,
                             VIR_DOMAIN_PAUSED_SAVE);
        dom->def->id = envId;
        break;
    case VMS_RESTORING:
        virDomainObjSetState(dom, VIR_DOMAIN_RUNNING,
                             VIR_DOMAIN_RUNNING_RESTORED);
        dom->def->id = envId;
        break;
    case VMS_CONTINUING:
        virDomainObjSetState(dom, VIR_DOMAIN_RUNNING,
                             VIR_DOMAIN_RUNNING_UNPAUSED);
        dom->def->id = envId;
        break;
    case VMS_RESUMING:
        virDomainObjSetState(dom, VIR_DOMAIN_RUNNING,
                             VIR_DOMAIN_RUNNING_RESTORED);
        dom->def->id = envId;
        break;
    case VMS_UNKNOWN:
        virDomainObjSetState(dom, VIR_DOMAIN_NOSTATE,
                             VIR_DOMAIN_NOSTATE_UNKNOWN);
        break;
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unknown domain state: %X"), domainState);
        return -1;
        break;
    }

    return 0;
}

static int
prlsdkConvertCpuInfo(PRL_HANDLE sdkdom,
                     virDomainDefPtr def)
{
    char *buf;
    PRL_UINT32 buflen = 0;
    int hostcpus;
    PRL_UINT32 cpuCount;
    PRL_RESULT pret;
    int ret = -1;

    if ((hostcpus = nodeGetCPUCount()) < 0)
        goto cleanup;

    /* get number of CPUs */
    pret = PrlVmCfg_GetCpuCount(sdkdom, &cpuCount);
    prlsdkCheckRetGoto(pret, cleanup);

    if (cpuCount > hostcpus)
        cpuCount = hostcpus;

    def->vcpus = cpuCount;
    def->maxvcpus = cpuCount;

    pret = PrlVmCfg_GetCpuMask(sdkdom, NULL, &buflen);
    prlsdkCheckRetGoto(pret, cleanup);

    if (VIR_ALLOC_N(buf, buflen) < 0)
        goto cleanup;

    pret = PrlVmCfg_GetCpuMask(sdkdom, buf, &buflen);

    if (strlen(buf) == 0) {
        if (!(def->cpumask = virBitmapNew(hostcpus)))
            goto cleanup;
        virBitmapSetAll(def->cpumask);
    } else {
        if (virBitmapParse(buf, 0, &def->cpumask, hostcpus) < 0)
            goto cleanup;
    }

    ret = 0;
 cleanup:
    VIR_FREE(buf);
    return ret;
}

static int
prlsdkConvertDomainType(PRL_HANDLE sdkdom, virDomainDefPtr def)
{
    PRL_VM_TYPE domainType;
    PRL_RESULT pret;

    pret = PrlVmCfg_GetVmType(sdkdom, &domainType);
    prlsdkCheckRetGoto(pret, error);

    switch (domainType) {
    case PVT_VM:
        def->os.type = VIR_DOMAIN_OSTYPE_HVM;
        break;
    case PVT_CT:
        def->os.type = VIR_DOMAIN_OSTYPE_EXE;
        if (VIR_STRDUP(def->os.init, "/sbin/init") < 0)
            return -1;
        break;
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unknown domain type: %X"), domainType);
        return -1;
    }

    return 0;

 error:
    return -1;
}

static int
prlsdkConvertCpuMode(PRL_HANDLE sdkdom, virDomainDefPtr def)
{
    PRL_RESULT pret;
    PRL_CPU_MODE cpuMode;

    pret = PrlVmCfg_GetCpuMode(sdkdom, &cpuMode);
    prlsdkCheckRetGoto(pret, error);

    switch (cpuMode) {
    case PCM_CPU_MODE_32:
        def->os.arch = VIR_ARCH_I686;
        break;
    case PCM_CPU_MODE_64:
        def->os.arch = VIR_ARCH_X86_64;
        break;
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unknown CPU mode: %X"), cpuMode);
        return -1;
    }

    return 0;
 error:
    return -1;
}

/*
 * This function retrieves information about domain.
 * If the domains is already in the domains list
 * privconn->domains, then locked 'olddom' must be
 * provided. If the domains must be added to the list,
 * olddom must be NULL.
 *
 * The function return a pointer to a locked virDomainObj.
 */
static virDomainObjPtr
prlsdkLoadDomain(parallelsConnPtr privconn,
                 PRL_HANDLE sdkdom,
                 virDomainObjPtr olddom)
{
    virDomainObjPtr dom = NULL;
    virDomainDefPtr def = NULL;
    parallelsDomObjPtr pdom = NULL;
    VIRTUAL_MACHINE_STATE domainState;

    PRL_UINT32 buflen = 0;
    PRL_RESULT pret;
    PRL_UINT32 ram;
    PRL_UINT32 envId;
    PRL_VM_AUTOSTART_OPTION autostart;

    virCheckNonNullArgGoto(privconn, error);
    virCheckNonNullArgGoto(sdkdom, error);

    if (!(def = virDomainDefNew()))
        goto error;

    if (!olddom) {
        if (VIR_ALLOC(pdom) < 0)
            goto error;
    } else {
        pdom = olddom->privateData;
    }

    def->virtType = VIR_DOMAIN_VIRT_PARALLELS;
    def->id = -1;

    /* we will remove this field in the near future, so let's set it
     * to NULL temporarily */
    pdom->uuid = NULL;

    if (prlsdkGetDomainIds(sdkdom, &def->name, def->uuid) < 0)
        goto error;

    def->onReboot = VIR_DOMAIN_LIFECYCLE_RESTART;
    def->onPoweroff = VIR_DOMAIN_LIFECYCLE_DESTROY;
    def->onCrash = VIR_DOMAIN_LIFECYCLE_CRASH_DESTROY;

    /* get RAM parameters */
    pret = PrlVmCfg_GetRamSize(sdkdom, &ram);
    prlsdkCheckRetGoto(pret, error);
    virDomainDefSetMemoryInitial(def, ram << 10); /* RAM size obtained in Mbytes,
                                                     convert to Kbytes */
    def->mem.cur_balloon = ram << 10;

    if (prlsdkConvertCpuInfo(sdkdom, def) < 0)
        goto error;

    if (prlsdkConvertCpuMode(sdkdom, def) < 0)
        goto error;

    if (prlsdkConvertDomainType(sdkdom, def) < 0)
        goto error;

    if (prlsdkAddDomainHardware(sdkdom, def) < 0)
        goto error;

    if (prlsdkAddVNCInfo(sdkdom, def) < 0)
        goto error;

    pret = PrlVmCfg_GetEnvId(sdkdom, &envId);
    prlsdkCheckRetGoto(pret, error);
    pdom->id = envId;

    buflen = 0;
    pret = PrlVmCfg_GetHomePath(sdkdom, NULL, &buflen);
    prlsdkCheckRetGoto(pret, error);

    VIR_FREE(pdom->home);
    if (VIR_ALLOC_N(pdom->home, buflen) < 0)
        goto error;

    pret = PrlVmCfg_GetHomePath(sdkdom, pdom->home, &buflen);
    prlsdkCheckRetGoto(pret, error);

    /* For VMs pdom->home is actually /directory/config.pvs */
    if (!IS_CT(def)) {
        /* Get rid of /config.pvs in path string */
        char *s = strrchr(pdom->home, '/');
        if (s)
            *s = '\0';
    }

    if (virDomainDefAddImplicitControllers(def) < 0)
        goto error;

    if (def->ngraphics > 0) {
        int bus = IS_CT(def) ? VIR_DOMAIN_INPUT_BUS_PARALLELS:
                                VIR_DOMAIN_INPUT_BUS_PS2;

        if (virDomainDefMaybeAddInput(def,
                                      VIR_DOMAIN_INPUT_TYPE_MOUSE,
                                      bus) < 0)
            goto error;

        if (virDomainDefMaybeAddInput(def,
                                      VIR_DOMAIN_INPUT_TYPE_KBD,
                                      bus) < 0)
            goto error;
    }

    if (olddom) {
        /* assign new virDomainDef without any checks */
        /* we can't use virDomainObjAssignDef, because it checks
         * for state and domain name */
        dom = olddom;
        virDomainDefFree(dom->def);
        dom->def = def;
    } else {
        if (!(dom = virDomainObjListAdd(privconn->domains, def,
                                        privconn->xmlopt,
                                        0, NULL)))
        goto error;
    }
    /* dom is locked here */

    dom->privateData = pdom;
    dom->privateDataFreeFunc = prlsdkDomObjFreePrivate;
    dom->persistent = 1;

    if (prlsdkGetDomainState(sdkdom, &domainState) < 0)
        goto error;

    if (prlsdkConvertDomainState(domainState, envId, dom) < 0)
        goto error;

    pret = PrlVmCfg_GetAutoStart(sdkdom, &autostart);
    prlsdkCheckRetGoto(pret, error);

    switch (autostart) {
    case PAO_VM_START_ON_LOAD:
        dom->autostart = 1;
        break;
    case PAO_VM_START_MANUAL:
        dom->autostart = 0;
        break;
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unknown autostart mode: %X"), autostart);
        goto error;
    }

    if (!pdom->sdkdom) {
        pret = PrlHandle_AddRef(sdkdom);
        prlsdkCheckRetGoto(pret, error);
        pdom->sdkdom = sdkdom;
    }

    return dom;
 error:
    if (dom && !olddom)
        virDomainObjListRemove(privconn->domains, dom);
    virDomainDefFree(def);
    prlsdkDomObjFreePrivate(pdom);
    return NULL;
}

int
prlsdkLoadDomains(parallelsConnPtr privconn)
{
    PRL_HANDLE job = PRL_INVALID_HANDLE;
    PRL_HANDLE result;
    PRL_HANDLE sdkdom;
    PRL_UINT32 paramsCount;
    PRL_RESULT pret;
    size_t i = 0;
    virDomainObjPtr dom;

    job = PrlSrv_GetVmListEx(privconn->server, PVTF_VM | PVTF_CT);

    if (PRL_FAILED(getJobResult(job, &result)))
        return -1;

    pret = PrlResult_GetParamsCount(result, &paramsCount);
    prlsdkCheckRetGoto(pret, error);

    for (i = 0; i < paramsCount; i++) {
        pret = PrlResult_GetParamByIndex(result, i, &sdkdom);
        if (PRL_FAILED(pret)) {
            logPrlError(pret);
            PrlHandle_Free(sdkdom);
            goto error;
        }

        dom = prlsdkLoadDomain(privconn, sdkdom, NULL);
        PrlHandle_Free(sdkdom);

        if (!dom)
            goto error;
        else
            virObjectUnlock(dom);
    }

    PrlHandle_Free(result);
    return 0;

 error:
    PrlHandle_Free(result);
    return -1;
}

virDomainObjPtr
prlsdkAddDomain(parallelsConnPtr privconn, const unsigned char *uuid)
{
    PRL_HANDLE sdkdom = PRL_INVALID_HANDLE;
    virDomainObjPtr dom;

    dom = virDomainObjListFindByUUID(privconn->domains, uuid);
    if (dom) {
        /* domain is already in the list */
        return dom;
    }

    sdkdom = prlsdkSdkDomainLookupByUUID(privconn, uuid);
    if (sdkdom == PRL_INVALID_HANDLE)
        return NULL;

    dom = prlsdkLoadDomain(privconn, sdkdom, NULL);
    PrlHandle_Free(sdkdom);
    return dom;
}

int
prlsdkUpdateDomain(parallelsConnPtr privconn, virDomainObjPtr dom)
{
    PRL_HANDLE job;
    virDomainObjPtr retdom = NULL;
    parallelsDomObjPtr pdom = dom->privateData;

    job = PrlVm_RefreshConfig(pdom->sdkdom);
    if (waitJob(job))
        return -1;

    retdom = prlsdkLoadDomain(privconn, pdom->sdkdom, dom);
    return retdom ? 0 : -1;
}

static int prlsdkSendEvent(parallelsConnPtr privconn,
                           virDomainObjPtr dom,
                           virDomainEventType lvEventType,
                           int lvEventTypeDetails)
{
    virObjectEventPtr event = NULL;

    event = virDomainEventLifecycleNewFromObj(dom,
                                              lvEventType,
                                              lvEventTypeDetails);
    if (!event)
        return -1;

    virObjectEventStateQueue(privconn->domainEventState, event);
    return 0;
}

static void
prlsdkNewStateToEvent(VIRTUAL_MACHINE_STATE domainState,
                      virDomainEventType *lvEventType,
                      int *lvEventTypeDetails)
{
    /* We skip all intermediate states here, because
     * libvirt doesn't have correspoding event types for
     * them */
    switch (domainState) {
    case VMS_STOPPED:
    case VMS_MOUNTED:
        *lvEventType = VIR_DOMAIN_EVENT_STOPPED;
        *lvEventTypeDetails = VIR_DOMAIN_EVENT_STOPPED_SHUTDOWN;
        break;
    case VMS_RUNNING:
        *lvEventType = VIR_DOMAIN_EVENT_STARTED;
        *lvEventTypeDetails = VIR_DOMAIN_EVENT_STARTED_BOOTED;
        break;
    case VMS_PAUSED:
        *lvEventType = VIR_DOMAIN_EVENT_SUSPENDED;
        *lvEventTypeDetails = VIR_DOMAIN_EVENT_SUSPENDED_PAUSED;
        break;
    case VMS_SUSPENDED:
        *lvEventType = VIR_DOMAIN_EVENT_STOPPED;
        *lvEventTypeDetails = VIR_DOMAIN_EVENT_STOPPED_SAVED;
        break;
    default:
        VIR_DEBUG("Skip sending event about changing state to %X",
                  domainState);
        break;
    }
}

static PRL_RESULT
prlsdkHandleVmStateEvent(parallelsConnPtr privconn,
                         PRL_HANDLE prlEvent,
                         unsigned char *uuid)
{
    PRL_RESULT pret = PRL_ERR_FAILURE;
    PRL_HANDLE eventParam = PRL_INVALID_HANDLE;
    PRL_INT32 domainState;
    virDomainObjPtr dom = NULL;
    parallelsDomObjPtr pdom;
    virDomainEventType lvEventType = 0;
    int lvEventTypeDetails = 0;

    pret = PrlEvent_GetParamByName(prlEvent, "vminfo_vm_state", &eventParam);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlEvtPrm_ToInt32(eventParam, &domainState);
    prlsdkCheckRetGoto(pret, cleanup);

    dom = virDomainObjListFindByUUID(privconn->domains, uuid);
    if (dom == NULL) {
        pret = PRL_ERR_VM_UUID_NOT_FOUND;
        goto cleanup;
    }

    pdom = dom->privateData;
    if (prlsdkConvertDomainState(domainState, pdom->id, dom) < 0)
        goto cleanup;

    prlsdkNewStateToEvent(domainState,
                          &lvEventType,
                          &lvEventTypeDetails);

    if (prlsdkSendEvent(privconn, dom, lvEventType, lvEventTypeDetails) < 0) {
        pret = PRL_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }

 cleanup:
    if (dom)
        virObjectUnlock(dom);
    return pret;
}

static PRL_RESULT
prlsdkHandleVmConfigEvent(parallelsConnPtr privconn,
                          unsigned char *uuid)
{
    PRL_RESULT pret = PRL_ERR_FAILURE;
    virDomainObjPtr dom = NULL;

    dom = virDomainObjListFindByUUID(privconn->domains, uuid);
    if (dom == NULL) {
        pret = PRL_ERR_VM_UUID_NOT_FOUND;
        goto cleanup;
    }

    if (prlsdkUpdateDomain(privconn, dom) < 0)
        goto cleanup;

    if (prlsdkSendEvent(privconn, dom, VIR_DOMAIN_EVENT_DEFINED,
                        VIR_DOMAIN_EVENT_DEFINED_UPDATED) < 0) {
        pret = PRL_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }

    pret = PRL_ERR_SUCCESS;
 cleanup:
    if (dom)
        virObjectUnlock(dom);
    return pret;
}

static PRL_RESULT
prlsdkHandleVmAddedEvent(parallelsConnPtr privconn,
                       unsigned char *uuid)
{
    PRL_RESULT pret;
    virDomainObjPtr dom = NULL;

    dom = prlsdkAddDomain(privconn, uuid);
    if (!dom)
        return PRL_ERR_FAILURE;

    if (prlsdkSendEvent(privconn, dom, VIR_DOMAIN_EVENT_DEFINED,
                        VIR_DOMAIN_EVENT_DEFINED_ADDED) < 0) {
        pret = PRL_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }

    pret = PRL_ERR_SUCCESS;
 cleanup:
    if (dom)
        virObjectUnlock(dom);
    return pret;
}

static PRL_RESULT
prlsdkHandleVmRemovedEvent(parallelsConnPtr privconn,
                           unsigned char *uuid)
{
    virDomainObjPtr dom = NULL;
    PRL_RESULT pret = PRL_ERR_SUCCESS;

    dom = virDomainObjListFindByUUID(privconn->domains, uuid);
    if (dom == NULL) {
        /* domain was removed from the list from the libvirt
         * API function in current connection */
        return PRL_ERR_SUCCESS;
    }

    if (prlsdkSendEvent(privconn, dom, VIR_DOMAIN_EVENT_UNDEFINED,
                        VIR_DOMAIN_EVENT_UNDEFINED_REMOVED) < 0)
        pret = PRL_ERR_OUT_OF_MEMORY;

    virDomainObjListRemove(privconn->domains, dom);
    return pret;
}

static PRL_RESULT
prlsdkHandleVmEvent(parallelsConnPtr privconn, PRL_HANDLE prlEvent)
{
    PRL_RESULT pret;
    char uuidstr[VIR_UUID_STRING_BUFLEN + 2];
    unsigned char uuid[VIR_UUID_BUFLEN];
    PRL_UINT32 bufsize = ARRAY_CARDINALITY(uuidstr);
    PRL_EVENT_TYPE prlEventType;

    pret = PrlEvent_GetType(prlEvent, &prlEventType);
    prlsdkCheckRetGoto(pret, error);

    pret = PrlEvent_GetIssuerId(prlEvent, uuidstr, &bufsize);
    prlsdkCheckRetGoto(pret, error);

    if (prlsdkUUIDParse(uuidstr, uuid) < 0)
        return PRL_ERR_FAILURE;

    switch (prlEventType) {
        case PET_DSP_EVT_VM_STATE_CHANGED:
            return prlsdkHandleVmStateEvent(privconn, prlEvent, uuid);
        case PET_DSP_EVT_VM_CONFIG_CHANGED:
            return prlsdkHandleVmConfigEvent(privconn, uuid);
        case PET_DSP_EVT_VM_CREATED:
        case PET_DSP_EVT_VM_ADDED:
            return prlsdkHandleVmAddedEvent(privconn, uuid);
        case PET_DSP_EVT_VM_DELETED:
        case PET_DSP_EVT_VM_UNREGISTERED:
            return prlsdkHandleVmRemovedEvent(privconn, uuid);
            break;
        default:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Can't handle event of type %d"), prlEventType);
            return PRL_ERR_FAILURE;
    }

 error:
    return PRL_ERR_FAILURE;
}

static PRL_RESULT
prlsdkEventsHandler(PRL_HANDLE prlEvent, PRL_VOID_PTR opaque)
{
    parallelsConnPtr privconn = opaque;
    PRL_RESULT pret = PRL_ERR_UNINITIALIZED;
    PRL_HANDLE_TYPE handleType;
    PRL_EVENT_TYPE prlEventType;

    pret = PrlHandle_GetType(prlEvent, &handleType);
    prlsdkCheckRetGoto(pret, cleanup);

    if (handleType != PHT_EVENT) {
        /* Currently, there is no need to handle anything but events */
        pret = PRL_ERR_SUCCESS;
        goto cleanup;
    }

    if (privconn == NULL) {
        pret = PRL_ERR_INVALID_ARG;
        goto cleanup;
    }

    PrlEvent_GetType(prlEvent, &prlEventType);
    prlsdkCheckRetGoto(pret, cleanup);

    switch (prlEventType) {
        case PET_DSP_EVT_VM_STATE_CHANGED:
        case PET_DSP_EVT_VM_CONFIG_CHANGED:
        case PET_DSP_EVT_VM_CREATED:
        case PET_DSP_EVT_VM_ADDED:
        case PET_DSP_EVT_VM_DELETED:
        case PET_DSP_EVT_VM_UNREGISTERED:
            pret = prlsdkHandleVmEvent(privconn, prlEvent);
            break;
        default:
            VIR_DEBUG("Skipping event of type %d", prlEventType);
    }

    pret = PRL_ERR_SUCCESS;
 cleanup:
    PrlHandle_Free(prlEvent);
    return pret;
}

int prlsdkSubscribeToPCSEvents(parallelsConnPtr privconn)
{
    PRL_RESULT pret = PRL_ERR_UNINITIALIZED;

    pret = PrlSrv_RegEventHandler(privconn->server,
                                 prlsdkEventsHandler,
                                 privconn);
    prlsdkCheckRetGoto(pret, error);
    return 0;

 error:
    return -1;
}

void prlsdkUnsubscribeFromPCSEvents(parallelsConnPtr privconn)
{
    PRL_RESULT ret = PRL_ERR_UNINITIALIZED;
    ret = PrlSrv_UnregEventHandler(privconn->server,
                                 prlsdkEventsHandler,
                                 privconn);
    if (PRL_FAILED(ret))
        logPrlError(ret);
}

PRL_RESULT prlsdkStart(PRL_HANDLE sdkdom)
{
    PRL_HANDLE job = PRL_INVALID_HANDLE;

    job = PrlVm_StartEx(sdkdom, PSM_VM_START, 0);
    return waitJob(job);
}

static PRL_RESULT prlsdkStopEx(PRL_HANDLE sdkdom, PRL_UINT32 mode)
{
    PRL_HANDLE job = PRL_INVALID_HANDLE;

    job = PrlVm_StopEx(sdkdom, mode, 0);
    return waitJob(job);
}

PRL_RESULT prlsdkKill(PRL_HANDLE sdkdom)
{
    return prlsdkStopEx(sdkdom, PSM_KILL);
}

PRL_RESULT prlsdkStop(PRL_HANDLE sdkdom)
{
    return prlsdkStopEx(sdkdom, PSM_SHUTDOWN);
}

PRL_RESULT prlsdkPause(PRL_HANDLE sdkdom)
{
    PRL_HANDLE job = PRL_INVALID_HANDLE;

    job = PrlVm_Pause(sdkdom, false);
    return waitJob(job);
}

PRL_RESULT prlsdkResume(PRL_HANDLE sdkdom)
{
    PRL_HANDLE job = PRL_INVALID_HANDLE;

    job = PrlVm_Resume(sdkdom);
    return waitJob(job);
}

PRL_RESULT prlsdkSuspend(PRL_HANDLE sdkdom)
{
    PRL_HANDLE job = PRL_INVALID_HANDLE;

    job = PrlVm_Suspend(sdkdom);
    return waitJob(job);
}

int
prlsdkDomainChangeStateLocked(parallelsConnPtr privconn,
                              virDomainObjPtr dom,
                              prlsdkChangeStateFunc chstate)
{
    parallelsDomObjPtr pdom;
    PRL_RESULT pret;
    virErrorNumber virerr;

    pdom = dom->privateData;
    pret = chstate(pdom->sdkdom);
    if (PRL_FAILED(pret)) {
        virResetLastError();

        switch (pret) {
        case PRL_ERR_DISP_VM_IS_NOT_STARTED:
        case PRL_ERR_DISP_VM_IS_NOT_STOPPED:
            virerr = VIR_ERR_OPERATION_INVALID;
            break;
        default:
            virerr = VIR_ERR_OPERATION_FAILED;
        }

        virReportError(virerr, "%s", _("Can't change domain state."));
        return -1;
    }

    return prlsdkUpdateDomain(privconn, dom);
}

int
prlsdkDomainChangeState(virDomainPtr domain,
                        prlsdkChangeStateFunc chstate)
{
    parallelsConnPtr privconn = domain->conn->privateData;
    virDomainObjPtr dom;
    int ret = -1;

    if (!(dom = parallelsDomObjFromDomain(domain)))
        return -1;

    ret = prlsdkDomainChangeStateLocked(privconn, dom, chstate);
    virObjectUnlock(dom);
    return ret;
}

static int
prlsdkCheckUnsupportedParams(PRL_HANDLE sdkdom, virDomainDefPtr def)
{
    size_t i;
    PRL_VM_TYPE vmType;
    PRL_RESULT pret;
    virDomainNumatuneMemMode memMode;

    if (def->title) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("titles are not supported by parallels driver"));
        return -1;
    }

    if (def->blkio.ndevices > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("blkio parameters are not supported "
                         "by parallels driver"));
        return -1;
    }

    if (virDomainDefGetMemoryActual(def) != def->mem.cur_balloon) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                   _("changing balloon parameters is not supported "
                     "by parallels driver"));
       return -1;
    }

    if (virDomainDefGetMemoryActual(def) % (1 << 10) != 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                   _("Memory size should be multiple of 1Mb."));
        return -1;
    }

    if (def->mem.nhugepages ||
        virMemoryLimitIsSet(def->mem.hard_limit) ||
        virMemoryLimitIsSet(def->mem.soft_limit) ||
        def->mem.min_guarantee ||
        virMemoryLimitIsSet(def->mem.swap_hard_limit)) {

        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Memory parameter is not supported "
                         "by parallels driver"));
        return -1;
    }

    if (def->vcpus != def->maxvcpus) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                   _("current vcpus must be equal to maxvcpus"));
        return -1;
    }

    if (def->placement_mode) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("changing cpu placement mode is not supported "
                         "by parallels driver"));
        return -1;
    }

    if (def->cputune.shares ||
        def->cputune.sharesSpecified ||
        def->cputune.period ||
        def->cputune.quota) {

        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("cputune is not supported by parallels driver"));
        return -1;
    }

    if (def->cputune.vcpupin) {
       for (i = 0; i < def->vcpus; i++) {
            if (!virBitmapEqual(def->cpumask,
                                def->cputune.vcpupin[i]->cpumask)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               "%s", _("vcpupin cpumask differs from default cpumask"));
                return -1;
            }
        }
    }


    /*
     * Though we don't support NUMA configuration at the moment
     * virDomainDefPtr always contain non zero NUMA configuration
     * So, just make sure this configuration does't differ from auto generated.
     */
    if ((virDomainNumatuneGetMode(def->numa, -1, &memMode) == 0 &&
         memMode == VIR_DOMAIN_NUMATUNE_MEM_STRICT) ||
         virDomainNumatuneHasPerNodeBinding(def->numa)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                        _("numa parameters are not supported "
                          "by parallels driver"));
        return -1;
    }

    if (def->onReboot != VIR_DOMAIN_LIFECYCLE_RESTART ||
        def->onPoweroff != VIR_DOMAIN_LIFECYCLE_DESTROY ||
        def->onCrash != VIR_DOMAIN_LIFECYCLE_CRASH_DESTROY) {

        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("on_reboot, on_poweroff and on_crash parameters "
                         "are not supported by parallels driver"));
        return -1;
    }

    /* we fill only type and arch fields in parallelsLoadDomain for
     * hvm type and also init for containers, so we can check that all
     * other paramenters are null and boot devices config is default */

    if (def->os.machine != NULL || def->os.bootmenu != 0 ||
        def->os.kernel != NULL || def->os.initrd != NULL ||
        def->os.cmdline != NULL || def->os.root != NULL ||
        def->os.loader != NULL || def->os.bootloader != NULL ||
        def->os.bootloaderArgs != NULL || def->os.smbios_mode != 0 ||
        def->os.bios.useserial != 0) {

        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("changing OS parameters is not supported "
                         "by parallels driver"));
        return -1;
    }

    pret = PrlVmCfg_GetVmType(sdkdom, &vmType);
    if (PRL_FAILED(pret)) {
        logPrlError(pret);
        return -1;
    }

    if (!(vmType == PVT_VM && !IS_CT(def)) &&
        !(vmType == PVT_CT && IS_CT(def))) {

        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("changing OS type is not supported "
                         "by parallels driver"));
        return -1;
    }

    if (!IS_CT(def)) {
        if (def->os.nBootDevs != 1 ||
            def->os.bootDevs[0] != VIR_DOMAIN_BOOT_DISK ||
            def->os.init != NULL || def->os.initargv != NULL) {

            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("changing OS parameters is not supported "
                             "by parallels driver"));
            return -1;
        }
    } else {
        if (def->os.nBootDevs != 0 ||
            !STREQ_NULLABLE(def->os.init, "/sbin/init") ||
            (def->os.initargv != NULL && def->os.initargv[0] != NULL)) {

            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("changing OS parameters is not supported "
                             "by parallels driver"));
            return -1;
        }
    }

    if (def->emulator) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("changing emulator is not supported "
                         "by parallels driver"));
        return -1;
    }

    for (i = 0; i < VIR_DOMAIN_FEATURE_LAST; i++) {
        if (def->features[i]) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("changing features is not supported "
                             "by parallels driver"));
            return -1;
        }
    }

    if (def->clock.offset != VIR_DOMAIN_CLOCK_OFFSET_UTC ||
        def->clock.ntimers != 0) {

        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("changing clock parameters is not supported "
                         "by parallels driver"));
        return -1;
    }

    if (!IS_CT(def) && def->nfss != 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Filesystems in VMs are not supported "
                         "by parallels driver"));
        return -1;
    }

    if (def->nsounds != 0 || def->nhostdevs != 0 ||
        def->nredirdevs != 0 || def->nsmartcards != 0 ||
        def->nparallels || def->nchannels != 0 ||
        def->nleases != 0 || def->nhubs != 0) {

        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("changing devices parameters is not supported "
                         "by parallels driver"));
        return -1;
    }

    /* there may be one auto-input */
    if (def->ninputs != 0 &&
        (def->ninputs != 2 &&
            def->inputs[0]->type != VIR_DOMAIN_INPUT_TYPE_MOUSE &&
            def->inputs[0]->bus != VIR_DOMAIN_INPUT_BUS_PS2 &&
            def->inputs[1]->type != VIR_DOMAIN_INPUT_TYPE_KBD &&
            def->inputs[1]->bus != VIR_DOMAIN_INPUT_BUS_PS2)) {

        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("changing input devices parameters is not supported "
                         "by parallels driver"));
        return -1;
    }

    return 0;
}

static int prlsdkClearDevices(PRL_HANDLE sdkdom)
{
    PRL_RESULT pret;
    PRL_UINT32 n, i;
    PRL_HANDLE devList;
    PRL_HANDLE dev;
    int ret = -1;

    pret = PrlVmCfg_SetVNCMode(sdkdom, PRD_DISABLED);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmCfg_GetAllDevices(sdkdom, &devList);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlHndlList_GetItemsCount(devList, &n);
    prlsdkCheckRetGoto(pret, cleanup);

    for (i = 0; i < n; i++) {
        pret = PrlHndlList_GetItem(devList, i, &dev);
        prlsdkCheckRetGoto(pret, cleanup);

        pret = PrlVmDev_Remove(dev);
        PrlHandle_Free(dev);
    }

    ret = 0;
 cleanup:
    PrlHandle_Free(devList);
    return ret;
}

static int
prlsdkRemoveBootDevices(PRL_HANDLE sdkdom)
{
    PRL_RESULT pret;
    PRL_UINT32 i, devCount;
    PRL_HANDLE dev = PRL_INVALID_HANDLE;
    PRL_DEVICE_TYPE devType;

    pret = PrlVmCfg_GetBootDevCount(sdkdom, &devCount);
    prlsdkCheckRetGoto(pret, error);

    for (i = 0; i < devCount; i++) {

        /* always get device by index 0, because device list resort after delete */
        pret = PrlVmCfg_GetBootDev(sdkdom, 0, &dev);
        prlsdkCheckRetGoto(pret, error);

        pret = PrlBootDev_GetType(dev, &devType);
        prlsdkCheckRetGoto(pret, error);

        pret = PrlBootDev_Remove(dev);
        prlsdkCheckRetGoto(pret, error);
    }

    return 0;

 error:
    return -1;
}

static int
prlsdkAddDeviceToBootList(PRL_HANDLE sdkdom,
                          PRL_UINT32 devIndex,
                          PRL_DEVICE_TYPE devType,
                          PRL_UINT32 bootSequence)
{
    PRL_RESULT pret;
    PRL_HANDLE bootDev = PRL_INVALID_HANDLE;

    pret = PrlVmCfg_CreateBootDev(sdkdom, &bootDev);
    prlsdkCheckRetGoto(pret, error);

    pret = PrlBootDev_SetIndex(bootDev, devIndex);
    prlsdkCheckRetGoto(pret, error);

    pret = PrlBootDev_SetType(bootDev, devType);
    prlsdkCheckRetGoto(pret, error);

    pret = PrlBootDev_SetSequenceIndex(bootDev, bootSequence);
    prlsdkCheckRetGoto(pret, error);

    pret = PrlBootDev_SetInUse(bootDev, PRL_TRUE);
    prlsdkCheckRetGoto(pret, error);

    return 0;

 error:
    if (bootDev != PRL_INVALID_HANDLE)
        PrlBootDev_Remove(bootDev);

    return -1;
}

static int prlsdkCheckGraphicsUnsupportedParams(virDomainDefPtr def)
{
    virDomainGraphicsDefPtr gr;

    if (def->ngraphics == 0)
        return 0;

    if (def->ngraphics > 1) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Parallels Cloud Server supports only "
                         "one VNC per domain."));
        return -1;
    }

    gr = def->graphics[0];

    if (gr->type != VIR_DOMAIN_GRAPHICS_TYPE_VNC) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Parallels Cloud Server supports only "
                         "VNC graphics."));
        return -1;
    }

    if (gr->data.vnc.websocket != 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Parallels Cloud Server doesn't support "
                         "websockets for VNC graphics."));
        return -1;
    }

    if (gr->data.vnc.keymap != 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Parallels Cloud Server doesn't support "
                         "keymap setting for VNC graphics."));
        return -1;
    }

    if (gr->data.vnc.sharePolicy == VIR_DOMAIN_GRAPHICS_VNC_SHARE_ALLOW_EXCLUSIVE) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Parallels Cloud Server doesn't support "
                         "exclusive share policy for VNC graphics."));
        return -1;
    }

    if (gr->data.vnc.socket) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Parallels Cloud Server doesn't support "
                         "VNC graphics over unix sockets."));
        return -1;
    }

    if (gr->data.vnc.auth.connected == VIR_DOMAIN_GRAPHICS_AUTH_CONNECTED_FAIL ||
            gr->data.vnc.auth.connected == VIR_DOMAIN_GRAPHICS_AUTH_CONNECTED_KEEP) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Parallels Cloud Server doesn't support "
                         "given action in case of password change."));
        return -1;
    }

    if (gr->data.vnc.auth.expires) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Parallels Cloud Server doesn't support "
                         "setting password expire time."));
        return -1;
    }

    if (gr->nListens > 1) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("Parallels driver doesn't support more than "
                         "one listening VNC server per domain"));
        return -1;
    }

    if (gr->nListens == 1 &&
        virDomainGraphicsListenGetType(gr, 0) != VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("Parallels driver supports only address-based VNC listening"));
        return -1;
    }

    return 0;
}

static int prlsdkCheckVideoUnsupportedParams(virDomainDefPtr def)
{
    virDomainVideoDefPtr v;

    if (IS_CT(def)) {
        if (def->nvideos == 0) {
            return 0;
        } else {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Video adapters are not supported "
                             "int containers."));
            return -1;
        }
    } else {
        if (def->nvideos != 1) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Parallels Cloud Server supports "
                             "only one video adapter."));
            return -1;
        }
    }

    v = def->videos[0];

    if (v->type != VIR_DOMAIN_VIDEO_TYPE_VGA) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Parallels Cloud Server supports "
                         "only VGA video adapters."));
        return -1;
    }

    if (v->heads != 1) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Parallels Cloud Server doesn't support "
                         "multihead video adapters."));
        return -1;
    }

    if (v->accel != NULL && (v->accel->support2d || v->accel->support3d)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Parallels Cloud Server doesn't support "
                         "setting video acceleration parameters."));
        return -1;
    }

    return 0;
}

static int prlsdkCheckSerialUnsupportedParams(virDomainChrDefPtr chr)
{
    if (chr->deviceType != VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Specified character device type is not supported "
                         "by parallels driver."));
        return -1;
    }

    if (chr->targetTypeAttr) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Specified character device target type is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (chr->source.type != VIR_DOMAIN_CHR_TYPE_DEV &&
        chr->source.type != VIR_DOMAIN_CHR_TYPE_FILE &&
        chr->source.type != VIR_DOMAIN_CHR_TYPE_UNIX) {


        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Specified character device source type is not "
                         "supported by Parallels Cloud Server."));
        return -1;
    }

    if (chr->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting device info for character devices is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (chr->nseclabels > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting security labels is not "
                         "supported by parallels driver."));
        return -1;
    }

    return 0;
}

static int prlsdkCheckNetUnsupportedParams(virDomainNetDefPtr net)
{
    if (net->type != VIR_DOMAIN_NET_TYPE_NETWORK &&
        net->type != VIR_DOMAIN_NET_TYPE_BRIDGE) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Specified network adapter type is not "
                         "supported by Parallels Cloud Server."));
        return -1;
    }

    if (net->backend.tap || net->backend.vhost) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Interface backend parameters are not "
                         "supported by parallels driver."));
        return -1;
    }

    if (net->data.network.portgroup) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Virtual network portgroups are not "
                         "supported by Parallels Cloud Server."));
        return -1;
    }

    if (net->tune.sndbuf_specified) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting interface sndbuf is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (net->script) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting interface script is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (net->ifname_guest) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting guest interface name is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (net->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting device info for network devices is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (net->filter) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting network filter is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (net->bandwidth) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting network bandwidth is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (net->vlan.trunk) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting up vlans is not "
                         "supported by parallels driver."));
        return -1;
    }

    return 0;
}

static int prlsdkCheckDiskUnsupportedParams(virDomainDiskDefPtr disk)
{
    if (disk->device != VIR_DOMAIN_DISK_DEVICE_DISK &&
        disk->device != VIR_DOMAIN_DISK_DEVICE_CDROM) {

        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Only hard disks and cdroms are supported "
                         "by parallels driver."));
        return -1;
    }

   if (disk->blockio.logical_block_size ||
       disk->blockio.physical_block_size) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting disk block sizes is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (disk->blkdeviotune.total_bytes_sec ||
        disk->blkdeviotune.read_bytes_sec ||
        disk->blkdeviotune.write_bytes_sec ||
        disk->blkdeviotune.total_iops_sec ||
        disk->blkdeviotune.read_iops_sec ||
        disk->blkdeviotune.write_iops_sec) {

        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting disk io limits is not "
                         "supported by parallels driver yet."));
        return -1;
    }

    if (disk->serial) {
        VIR_INFO("%s", _("Setting disk serial number is not "
                         "supported by parallels driver."));
    }

    if (disk->wwn) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting disk wwn id is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (disk->vendor) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting disk vendor is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (disk->product) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting disk product id is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (disk->error_policy != VIR_DOMAIN_DISK_ERROR_POLICY_DEFAULT) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting disk error policy is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (disk->iomode) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting disk io mode is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (disk->copy_on_read) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Disk copy_on_read is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (disk->startupPolicy != VIR_DOMAIN_STARTUP_POLICY_DEFAULT) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting up disk startup policy is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (disk->transient) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Transient disks are not "
                         "supported by parallels driver."));
        return -1;
    }

    if (disk->discard) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting up disk discard parameter is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (disk->iothread) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting up disk io thread # is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (disk->src->type != VIR_STORAGE_TYPE_FILE &&
        disk->src->type != VIR_STORAGE_TYPE_BLOCK) {

        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Only disk and block storage types are "
                         "supported by parallels driver."));
        return -1;

    }

    return 0;
}

static int prlsdkCheckFSUnsupportedParams(virDomainFSDefPtr fs)
{
    if (fs->type != VIR_DOMAIN_FS_TYPE_FILE) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Only file based filesystems are "
                         "supported by parallels driver."));
        return -1;
    }

    if (fs->fsdriver != VIR_DOMAIN_FS_DRIVER_TYPE_PLOOP) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Only ploop fs driver is "
                         "supported by parallels driver."));
        return -1;
    }

    if (fs->accessmode != VIR_DOMAIN_FS_ACCESSMODE_PASSTHROUGH) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Changing fs access mode is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (fs->wrpolicy != VIR_DOMAIN_FS_WRPOLICY_DEFAULT) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Changing fs write policy is not "
                         "supported by parallels driver."));
        return -1;
    }

    if (fs->format != VIR_STORAGE_FILE_PLOOP) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Only ploop disk images are "
                         "supported by parallels driver."));
        return -1;
    }

    if (fs->readonly) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting readonly for filesystems is "
                         "supported by parallels driver."));
        return -1;
    }

    if (fs->space_hard_limit || fs->space_soft_limit) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting fs quotas is not "
                         "supported by parallels driver."));
        return -1;
    }

    return 0;
}

static int prlsdkApplyGraphicsParams(PRL_HANDLE sdkdom, virDomainDefPtr def)
{
    virDomainGraphicsDefPtr gr;
    PRL_RESULT pret;
    int ret  = -1;
    const char *listenAddr = NULL;

    if (prlsdkCheckGraphicsUnsupportedParams(def))
        return -1;

    if (def->ngraphics == 0)
        return 0;

    gr = def->graphics[0];

    if (gr->data.vnc.autoport) {
        pret = PrlVmCfg_SetVNCMode(sdkdom, PRD_AUTO);
        prlsdkCheckRetGoto(pret, cleanup);
    } else {
        pret = PrlVmCfg_SetVNCMode(sdkdom, PRD_MANUAL);
        prlsdkCheckRetGoto(pret, cleanup);

        pret = PrlVmCfg_SetVNCPort(sdkdom, gr->data.vnc.port);
        prlsdkCheckRetGoto(pret, cleanup);
    }

    if (gr->nListens == 1) {
        listenAddr = virDomainGraphicsListenGetAddress(gr, 0);
        if (!listenAddr)
            goto cleanup;
        pret = PrlVmCfg_SetVNCHostName(sdkdom, listenAddr);
        prlsdkCheckRetGoto(pret, cleanup);
    }

    ret = 0;
 cleanup:
    return ret;
}

static int prlsdkApplyVideoParams(PRL_HANDLE sdkdom ATTRIBUTE_UNUSED, virDomainDefPtr def)
{
    PRL_RESULT pret;

    if (def->nvideos == 0)
        return 0;

    if (IS_CT(def)) {
        /* ignore video parameters */
        return 0;
    }

    if (prlsdkCheckVideoUnsupportedParams(def))
        return -1;

    pret = PrlVmCfg_SetVideoRamSize(sdkdom, def->videos[0]->vram >> 10);
    prlsdkCheckRetGoto(pret, error);

    return 0;
 error:
    return -1;
}

static int prlsdkAddSerial(PRL_HANDLE sdkdom, virDomainChrDefPtr chr)
{
    PRL_RESULT pret;
    PRL_HANDLE sdkchr = PRL_INVALID_HANDLE;
    PRL_VM_DEV_EMULATION_TYPE emutype;
    PRL_SERIAL_PORT_SOCKET_OPERATION_MODE socket_mode =
                                    PSP_SERIAL_SOCKET_SERVER;
    char *path;
    int ret = -1;

    if (prlsdkCheckSerialUnsupportedParams(chr) < 0)
        return -1;

    pret = PrlVmCfg_CreateVmDev(sdkdom, PDE_SERIAL_PORT, &sdkchr);
    prlsdkCheckRetGoto(pret, cleanup);

    switch (chr->source.type) {
    case VIR_DOMAIN_CHR_TYPE_DEV:
        emutype = PDT_USE_REAL_DEVICE;
        path = chr->source.data.file.path;
        break;
    case VIR_DOMAIN_CHR_TYPE_FILE:
        emutype = PDT_USE_OUTPUT_FILE;
        path = chr->source.data.file.path;
        break;
    case VIR_DOMAIN_CHR_TYPE_UNIX:
        emutype = PDT_USE_SERIAL_PORT_SOCKET_MODE;
        path = chr->source.data.nix.path;
        if (chr->source.data.nix.listen)
            socket_mode = PSP_SERIAL_SOCKET_SERVER;
        else
            socket_mode = PSP_SERIAL_SOCKET_CLIENT;
        break;
    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Parallels Cloud Server doesn't support "
                         "specified serial source type."));
        goto cleanup;
    }

    pret = PrlVmDev_SetEmulatedType(sdkchr, emutype);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_SetSysName(sdkchr, path);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_SetFriendlyName(sdkchr, path);
    prlsdkCheckRetGoto(pret, cleanup);

    if (chr->source.type == VIR_DOMAIN_CHR_TYPE_UNIX) {
        pret = PrlVmDevSerial_SetSocketMode(sdkchr, socket_mode);
        prlsdkCheckRetGoto(pret, cleanup);
    }

    pret = PrlVmDev_SetEnabled(sdkchr, 1);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_SetIndex(sdkchr, chr->target.port);
    prlsdkCheckRetGoto(pret, cleanup);

    ret = 0;
 cleanup:
    PrlHandle_Free(sdkchr);
    return ret;
}

#define PRL_MAC_STRING_BUFNAME  13

static const char * prlsdkFormatMac(virMacAddrPtr mac, char *macstr)
{
    snprintf(macstr, PRL_MAC_STRING_BUFNAME,
             "%02X%02X%02X%02X%02X%02X",
             mac->addr[0], mac->addr[1], mac->addr[2],
             mac->addr[3], mac->addr[4], mac->addr[5]);
    macstr[PRL_MAC_STRING_BUFNAME - 1] = '\0';
    return macstr;
}

static int prlsdkAddNet(PRL_HANDLE sdkdom,
                        parallelsConnPtr privconn,
                        virDomainNetDefPtr net,
                        bool isCt)
{
    PRL_RESULT pret;
    PRL_HANDLE sdknet = PRL_INVALID_HANDLE;
    PRL_HANDLE vnet = PRL_INVALID_HANDLE;
    PRL_HANDLE job = PRL_INVALID_HANDLE;
    int ret = -1;
    char macstr[PRL_MAC_STRING_BUFNAME];

    if (prlsdkCheckNetUnsupportedParams(net) < 0)
        return -1;

    pret = PrlVmCfg_CreateVmDev(sdkdom, PDE_GENERIC_NETWORK_ADAPTER, &sdknet);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_SetEnabled(sdknet, 1);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_SetConnected(sdknet, net->linkstate !=
                                 VIR_DOMAIN_NET_INTERFACE_LINK_STATE_DOWN);

    prlsdkCheckRetGoto(pret, cleanup);

    if (net->ifname) {
        pret = PrlVmDevNet_SetHostInterfaceName(sdknet, net->ifname);
        prlsdkCheckRetGoto(pret, cleanup);
    }

    prlsdkFormatMac(&net->mac, macstr);
    pret = PrlVmDevNet_SetMacAddress(sdknet, macstr);
    prlsdkCheckRetGoto(pret, cleanup);

    if (isCt) {
        if (net->model)
             VIR_WARN("Setting network adapter for containers is not "
                      "supported by Parallels Cloud Server.");
    } else {
        if (STREQ(net->model, "rtl8139")) {
            pret = PrlVmDevNet_SetAdapterType(sdknet, PNT_RTL);
        } else if (STREQ(net->model, "e1000")) {
            pret = PrlVmDevNet_SetAdapterType(sdknet, PNT_E1000);
        } else if (STREQ(net->model, "virtio")) {
            pret = PrlVmDevNet_SetAdapterType(sdknet, PNT_VIRTIO);
        } else {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Specified network adapter model is not "
                         "supported by Parallels Cloud Server."));
            goto cleanup;
        }
        prlsdkCheckRetGoto(pret, cleanup);
    }

    if (net->type == VIR_DOMAIN_NET_TYPE_NETWORK) {
        if (STREQ(net->data.network.name, PARALLELS_DOMAIN_ROUTED_NETWORK_NAME)) {
            pret = PrlVmDev_SetEmulatedType(sdknet, PNA_ROUTED);
            prlsdkCheckRetGoto(pret, cleanup);
        } else if (STREQ(net->data.network.name, PARALLELS_DOMAIN_BRIDGED_NETWORK_NAME)) {
            pret = PrlVmDev_SetEmulatedType(sdknet, PNA_BRIDGED_ETHERNET);
            prlsdkCheckRetGoto(pret, cleanup);

            pret = PrlVmDevNet_SetVirtualNetworkId(sdknet, net->data.network.name);
            prlsdkCheckRetGoto(pret, cleanup);
        }
    } else if (net->type == VIR_DOMAIN_NET_TYPE_BRIDGE) {
        /*
         * For this type of adapter we create a new
         * Virtual Network assuming that bridge with given name exists
         * Failing creating this means domain creation failure
         */
        pret = PrlVirtNet_Create(&vnet);
        prlsdkCheckRetGoto(pret, cleanup);

        pret = PrlVirtNet_SetNetworkId(vnet, net->data.network.name);
        prlsdkCheckRetGoto(pret, cleanup);

        pret = PrlVirtNet_SetNetworkType(vnet, PVN_BRIDGED_ETHERNET);
        prlsdkCheckRetGoto(pret, cleanup);

        job = PrlSrv_AddVirtualNetwork(privconn->server, vnet, 0);
        if (PRL_FAILED(pret = waitJob(job)))
            goto cleanup;

        pret = PrlVmDev_SetEmulatedType(sdknet, PNA_BRIDGED_ETHERNET);
        prlsdkCheckRetGoto(pret, cleanup);

        pret = PrlVmDevNet_SetVirtualNetworkId(sdknet, net->data.network.name);
        prlsdkCheckRetGoto(pret, cleanup);
    }

    if (net->trustGuestRxFilters == VIR_TRISTATE_BOOL_YES)
        pret = PrlVmDevNet_SetPktFilterPreventMacSpoof(sdknet, 0);
    else if (net->trustGuestRxFilters == VIR_TRISTATE_BOOL_NO)
        pret = PrlVmDevNet_SetPktFilterPreventMacSpoof(sdknet, 1);
    prlsdkCheckRetGoto(pret, cleanup);

    ret = 0;
 cleanup:
    PrlHandle_Free(vnet);
    PrlHandle_Free(sdknet);
    return ret;
}

static void prlsdkDelNet(parallelsConnPtr privconn, virDomainNetDefPtr net)
{
    PRL_RESULT pret;
    PRL_HANDLE vnet = PRL_INVALID_HANDLE;
    PRL_HANDLE job = PRL_INVALID_HANDLE;

    if (net->type != VIR_DOMAIN_NET_TYPE_BRIDGE)
        return;

    pret = PrlVirtNet_Create(&vnet);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVirtNet_SetNetworkId(vnet, net->data.network.name);
    prlsdkCheckRetGoto(pret, cleanup);

    PrlSrv_DeleteVirtualNetwork(privconn->server, vnet, 0);
    if (PRL_FAILED(pret = waitJob(job)))
        goto cleanup;

 cleanup:
    PrlHandle_Free(vnet);
}

static int prlsdkDelDisk(PRL_HANDLE sdkdom, int idx)
{
    int ret = -1;
    PRL_RESULT pret;
    PRL_HANDLE sdkdisk = PRL_INVALID_HANDLE;

    pret = PrlVmCfg_GetHardDisk(sdkdom, idx, &sdkdisk);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_Remove(sdkdisk);
    prlsdkCheckRetGoto(pret, cleanup);

    ret = 0;

 cleanup:
    PrlHandle_Free(sdkdisk);
    return ret;
}

static int prlsdkAddDisk(PRL_HANDLE sdkdom, virDomainDiskDefPtr disk, bool bootDisk)
{
    PRL_RESULT pret;
    PRL_HANDLE sdkdisk = PRL_INVALID_HANDLE;
    int ret = -1;
    PRL_VM_DEV_EMULATION_TYPE emutype;
    PRL_MASS_STORAGE_INTERFACE_TYPE sdkbus;
    int idx;
    virDomainDeviceDriveAddressPtr drive;
    PRL_UINT32 devIndex;
    PRL_DEVICE_TYPE devType;
    char *dst = NULL;

    if (prlsdkCheckDiskUnsupportedParams(disk) < 0)
        return -1;

    if (disk->device == VIR_DOMAIN_DISK_DEVICE_DISK)
        devType = PDE_HARD_DISK;
    else
        devType = PDE_OPTICAL_DISK;

    pret = PrlVmCfg_CreateVmDev(sdkdom, devType, &sdkdisk);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_SetEnabled(sdkdisk, 1);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_SetConnected(sdkdisk, 1);
    prlsdkCheckRetGoto(pret, cleanup);

    if (disk->src->type == VIR_STORAGE_TYPE_FILE) {
        if (disk->device == VIR_DOMAIN_DISK_DEVICE_DISK &&
            virDomainDiskGetFormat(disk) != VIR_STORAGE_FILE_PLOOP) {

            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, _("Invalid format of "
                           "disk %s, Parallels Cloud Server supports only "
                           "images in ploop format."), disk->src->path);
            goto cleanup;
        }

        emutype = PDT_USE_IMAGE_FILE;
    } else {
        if (disk->device == VIR_DOMAIN_DISK_DEVICE_DISK &&
            (virDomainDiskGetFormat(disk) != VIR_STORAGE_FILE_RAW &&
             virDomainDiskGetFormat(disk) != VIR_STORAGE_FILE_NONE &&
             virDomainDiskGetFormat(disk) != VIR_STORAGE_FILE_AUTO)) {

            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, _("Invalid format "
                           "of disk %s, it should be either not set, or set "
                           "to raw or auto."), disk->src->path);
            goto cleanup;
        }
        emutype = PDT_USE_REAL_DEVICE;
    }

    pret = PrlVmDev_SetEmulatedType(sdkdisk, emutype);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_SetSysName(sdkdisk, disk->src->path);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_SetFriendlyName(sdkdisk, disk->src->path);
    prlsdkCheckRetGoto(pret, cleanup);

    drive = &disk->info.addr.drive;
    if (drive->controller > 0) {
        /* We have only one controller of each type */
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, _("Invalid drive "
                       "address of disk %s, Parallels Cloud Server has "
                       "only one controller."), disk->dst);
        goto cleanup;
    }

    if (drive->target > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, _("Invalid drive "
                       "address of disk %s, Parallels Cloud Server has "
                       "only target 0."), disk->dst);
        goto cleanup;
    }

    switch (disk->bus) {
    case VIR_DOMAIN_DISK_BUS_IDE:
        if (drive->unit > 1) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, _("Invalid drive "
                           "address of disk %s, Parallels Cloud Server has "
                           "only units 0-1 for IDE bus."), disk->dst);
            goto cleanup;
        }
        sdkbus = PMS_IDE_DEVICE;
        idx = 2 * drive->bus + drive->unit;
        dst = virIndexToDiskName(idx, "hd");
        break;
    case VIR_DOMAIN_DISK_BUS_SCSI:
        if (drive->bus > 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, _("Invalid drive "
                           "address of disk %s, Parallels Cloud Server has "
                           "only bus 0 for SCSI bus."), disk->dst);
            goto cleanup;
        }
        sdkbus = PMS_SCSI_DEVICE;
        idx = drive->unit;
        dst = virIndexToDiskName(idx, "sd");
        break;
    case VIR_DOMAIN_DISK_BUS_SATA:
        if (drive->bus > 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, _("Invalid drive "
                           "address of disk %s, Parallels Cloud Server has "
                           "only bus 0 for SATA bus."), disk->dst);
            goto cleanup;
        }
        sdkbus = PMS_SATA_DEVICE;
        idx = drive->unit;
        dst = virIndexToDiskName(idx, "sd");
        break;
    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Specified disk bus is not "
                         "supported by Parallels Cloud Server."));
        goto cleanup;
    }

    if (!dst)
        goto cleanup;

    if (STRNEQ(dst, disk->dst)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, _("Invalid drive "
                       "address of disk %s, Parallels Cloud Server supports "
                       "only defaults address to logical device name."), disk->dst);
        goto cleanup;
    }

    pret = PrlVmDev_SetIfaceType(sdkdisk, sdkbus);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_SetStackIndex(sdkdisk, idx);
    prlsdkCheckRetGoto(pret, cleanup);

    switch (disk->cachemode) {
    case VIR_DOMAIN_DISK_CACHE_DISABLE:
        pret = PrlVmCfg_SetDiskCacheWriteBack(sdkdom, PRL_FALSE);
        prlsdkCheckRetGoto(pret, cleanup);
        break;
    case VIR_DOMAIN_DISK_CACHE_WRITEBACK:
        pret = PrlVmCfg_SetDiskCacheWriteBack(sdkdom, PRL_TRUE);
        prlsdkCheckRetGoto(pret, cleanup);
        break;
    case VIR_DOMAIN_DISK_CACHE_DEFAULT:
        break;
    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Specified disk cache mode is not "
                         "supported by Parallels Cloud Server."));
        goto cleanup;
    }

    if (bootDisk == true) {
        pret = PrlVmDev_GetIndex(sdkdisk, &devIndex);
        prlsdkCheckRetGoto(pret, cleanup);

        if (prlsdkAddDeviceToBootList(sdkdom, devIndex, devType, 0) < 0)
            goto cleanup;
    }

    return 0;
 cleanup:
    PrlHandle_Free(sdkdisk);
    VIR_FREE(dst);
    return ret;
}

int
prlsdkAttachVolume(virDomainObjPtr dom, virDomainDiskDefPtr disk)
{
    int ret = -1;
    parallelsDomObjPtr privdom = dom->privateData;
    PRL_HANDLE job = PRL_INVALID_HANDLE;

    job = PrlVm_BeginEdit(privdom->sdkdom);
    if (PRL_FAILED(waitJob(job)))
        goto cleanup;

    ret = prlsdkAddDisk(privdom->sdkdom, disk, false);
    if (ret == 0) {
        job = PrlVm_CommitEx(privdom->sdkdom, PVCF_DETACH_HDD_BUNDLE);
        if (PRL_FAILED(waitJob(job))) {
            ret = -1;
            goto cleanup;
        }
    }

 cleanup:
    return ret;
}

static int
prlsdkGetDiskIndex(PRL_HANDLE sdkdom, virDomainDiskDefPtr disk)
{
    int idx = -1;
    char *buf = NULL;
    PRL_UINT32 buflen = 0;
    PRL_RESULT pret;
    PRL_UINT32 hddCount;
    PRL_UINT32 i;
    PRL_HANDLE hdd = PRL_INVALID_HANDLE;

    pret = PrlVmCfg_GetHardDisksCount(sdkdom, &hddCount);
    prlsdkCheckRetGoto(pret, cleanup);

    for (i = 0; i < hddCount; ++i) {

        pret = PrlVmCfg_GetHardDisk(sdkdom, i, &hdd);
        prlsdkCheckRetGoto(pret, cleanup);

        pret = PrlVmDev_GetFriendlyName(hdd, 0, &buflen);
        prlsdkCheckRetGoto(pret, cleanup);

        if (VIR_ALLOC_N(buf, buflen) < 0)
            goto cleanup;

        pret = PrlVmDev_GetFriendlyName(hdd, buf, &buflen);
        prlsdkCheckRetGoto(pret, cleanup);

        if (STRNEQ(disk->src->path, buf)) {

            PrlHandle_Free(hdd);
            hdd = PRL_INVALID_HANDLE;
            VIR_FREE(buf);
            continue;
        }

        VIR_FREE(buf);
        idx = i;
        break;
    }

 cleanup:
    PrlHandle_Free(hdd);
    return idx;
}

int
prlsdkDetachVolume(virDomainObjPtr dom, virDomainDiskDefPtr disk)
{
    int ret = -1, idx;
    parallelsDomObjPtr privdom = dom->privateData;
    PRL_HANDLE job = PRL_INVALID_HANDLE;

    idx = prlsdkGetDiskIndex(privdom->sdkdom, disk);
    if (idx < 0)
        goto cleanup;

    job = PrlVm_BeginEdit(privdom->sdkdom);
    if (PRL_FAILED(waitJob(job)))
        goto cleanup;

    ret = prlsdkDelDisk(privdom->sdkdom, idx);
    if (ret == 0) {
        job = PrlVm_CommitEx(privdom->sdkdom, PVCF_DETACH_HDD_BUNDLE);
        if (PRL_FAILED(waitJob(job))) {
            ret = -1;
            goto cleanup;
        }
    }

 cleanup:
    return ret;
}

static int
prlsdkAddFS(PRL_HANDLE sdkdom, virDomainFSDefPtr fs)
{
    PRL_RESULT pret;
    PRL_HANDLE sdkdisk = PRL_INVALID_HANDLE;
    int ret = -1;

    if (prlsdkCheckFSUnsupportedParams(fs) < 0)
        return -1;

    pret = PrlVmCfg_CreateVmDev(sdkdom, PDE_HARD_DISK, &sdkdisk);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_SetEnabled(sdkdisk, 1);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_SetConnected(sdkdisk, 1);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_SetEmulatedType(sdkdisk, PDT_USE_IMAGE_FILE);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_SetSysName(sdkdisk, fs->src);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_SetImagePath(sdkdisk, fs->src);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDev_SetFriendlyName(sdkdisk, fs->src);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmDevHd_SetMountPoint(sdkdisk, fs->dst);
    prlsdkCheckRetGoto(pret, cleanup);

    ret = 0;

 cleanup:
    PrlHandle_Free(sdkdisk);
    return ret;
}
static int
prlsdkDoApplyConfig(virConnectPtr conn,
                    PRL_HANDLE sdkdom,
                    virDomainDefPtr def,
                    virDomainDefPtr olddef)
{
    PRL_RESULT pret;
    size_t i;
    char uuidstr[VIR_UUID_STRING_BUFLEN + 2];
    bool needBoot = true;
    char *mask = NULL;

    if (prlsdkCheckUnsupportedParams(sdkdom, def) < 0)
        return -1;

    if (def->description) {
        pret = PrlVmCfg_SetDescription(sdkdom, def->description);
        prlsdkCheckRetGoto(pret, error);
    }

    if (def->name) {
        pret = PrlVmCfg_SetName(sdkdom, def->name);
        prlsdkCheckRetGoto(pret, error);
    }

    if (def->uuid) {
        prlsdkUUIDFormat(def->uuid, uuidstr);

        pret = PrlVmCfg_SetUuid(sdkdom, uuidstr);
        prlsdkCheckRetGoto(pret, error);
    }

    pret = PrlVmCfg_SetRamSize(sdkdom, virDomainDefGetMemoryActual(def) >> 10);
    prlsdkCheckRetGoto(pret, error);

    pret = PrlVmCfg_SetCpuCount(sdkdom, def->vcpus);
    prlsdkCheckRetGoto(pret, error);

    if (!(mask = virBitmapFormat(def->cpumask)))
        goto error;

    pret = PrlVmCfg_SetCpuMask(sdkdom, mask);
    prlsdkCheckRetGoto(pret, error);
    VIR_FREE(mask);

    switch (def->os.arch) {
        case VIR_ARCH_X86_64:
            pret = PrlVmCfg_SetCpuMode(sdkdom, PCM_CPU_MODE_64);
            break;
        case VIR_ARCH_I686:
            pret = PrlVmCfg_SetCpuMode(sdkdom, PCM_CPU_MODE_32);
            break;
        default:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown CPU mode: %s"),
                           virArchToString(def->os.arch));
            goto error;
    }
    prlsdkCheckRetGoto(pret, error);

    if (prlsdkClearDevices(sdkdom) < 0)
        goto error;

    if (prlsdkRemoveBootDevices(sdkdom) < 0)
        goto error;

    if (olddef) {
        for (i = 0; i < olddef->nnets; i++)
            prlsdkDelNet(conn->privateData, olddef->nets[i]);
    }

    for (i = 0; i < def->nnets; i++) {
        if (prlsdkAddNet(sdkdom, conn->privateData, def->nets[i], IS_CT(def)) < 0)
           goto error;
    }

    if (prlsdkApplyGraphicsParams(sdkdom, def) < 0)
        goto error;

    if (prlsdkApplyVideoParams(sdkdom, def) < 0)
        goto error;

    for (i = 0; i < def->nserials; i++) {
        if (prlsdkAddSerial(sdkdom, def->serials[i]) < 0)
            goto error;
    }

    for (i = 0; i < def->ndisks; i++) {
        bool bootDisk = false;

        if (needBoot == true &&
            def->disks[i]->device == VIR_DOMAIN_DISK_DEVICE_DISK) {

            needBoot = false;
            bootDisk = true;
        }
        if (prlsdkAddDisk(sdkdom, def->disks[i], bootDisk) < 0)
            goto error;
    }

    for (i = 0; i < def->nfss; i++) {
        if (prlsdkAddFS(sdkdom, def->fss[i]) < 0)
            goto error;
    }

    return 0;

 error:
    VIR_FREE(mask);

    for (i = 0; i < def->nnets; i++)
        prlsdkDelNet(conn->privateData, def->nets[i]);

   return -1;
}

int
prlsdkApplyConfig(virConnectPtr conn,
                  virDomainObjPtr dom,
                  virDomainDefPtr new)
{
    parallelsConnPtr privconn = conn->privateData;
    PRL_HANDLE sdkdom = PRL_INVALID_HANDLE;
    PRL_HANDLE job = PRL_INVALID_HANDLE;
    int ret;

    sdkdom = prlsdkSdkDomainLookupByUUID(privconn, dom->def->uuid);
    if (sdkdom == PRL_INVALID_HANDLE)
        return -1;

    job = PrlVm_BeginEdit(sdkdom);
    if (PRL_FAILED(waitJob(job)))
        return -1;

    ret = prlsdkDoApplyConfig(conn, sdkdom, new, dom->def);

    if (ret == 0) {
        job = PrlVm_CommitEx(sdkdom, PVCF_DETACH_HDD_BUNDLE);
        if (PRL_FAILED(waitJob(job)))
            ret = -1;
    }

    PrlHandle_Free(sdkdom);

    return ret;
}

int
prlsdkCreateVm(virConnectPtr conn, virDomainDefPtr def)
{
    parallelsConnPtr privconn = conn->privateData;
    PRL_HANDLE sdkdom = PRL_INVALID_HANDLE;
    PRL_HANDLE job = PRL_INVALID_HANDLE;
    PRL_HANDLE result = PRL_INVALID_HANDLE;
    PRL_HANDLE srvconf = PRL_INVALID_HANDLE;
    PRL_RESULT pret;
    int ret = -1;

    pret = PrlSrv_CreateVm(privconn->server, &sdkdom);
    prlsdkCheckRetGoto(pret, cleanup);

    job = PrlSrv_GetSrvConfig(privconn->server);
    if (PRL_FAILED(getJobResult(job, &result)))
        goto cleanup;

    pret = PrlResult_GetParamByIndex(result, 0, &srvconf);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmCfg_SetDefaultConfig(sdkdom, srvconf, PVS_GUEST_VER_LIN_REDHAT, 0);
    prlsdkCheckRetGoto(pret, cleanup);

    pret = PrlVmCfg_SetOfflineManagementEnabled(sdkdom, 0);
    prlsdkCheckRetGoto(pret, cleanup);

    ret = prlsdkDoApplyConfig(conn, sdkdom, def, NULL);
    if (ret)
        goto cleanup;

    job = PrlVm_Reg(sdkdom, "", 1);
    if (PRL_FAILED(waitJob(job)))
        ret = -1;

 cleanup:
    PrlHandle_Free(sdkdom);
    return ret;
}

int
prlsdkCreateCt(virConnectPtr conn, virDomainDefPtr def)
{
    parallelsConnPtr privconn = conn->privateData;
    PRL_HANDLE sdkdom = PRL_INVALID_HANDLE;
    PRL_GET_VM_CONFIG_PARAM_DATA confParam;
    PRL_HANDLE job = PRL_INVALID_HANDLE;
    PRL_HANDLE result = PRL_INVALID_HANDLE;
    PRL_RESULT pret;
    int ret = -1;
    int useTemplate = 0;
    size_t i;

    if (def->nfss > 1) {
        /* Check all filesystems */
        for (i = 0; i < def->nfss; i++) {
            if (def->fss[i]->type != VIR_DOMAIN_FS_TYPE_FILE) {
                virReportError(VIR_ERR_INVALID_ARG, "%s",
                               _("Unsupported filesystem type."));
                return -1;
            }
        }
    } else if (def->nfss == 1) {
        if (def->fss[0]->type == VIR_DOMAIN_FS_TYPE_TEMPLATE) {
            useTemplate = 1;
        } else if (def->fss[0]->type != VIR_DOMAIN_FS_TYPE_FILE) {
            virReportError(VIR_ERR_INVALID_ARG, "%s",
                           _("Unsupported filesystem type."));
            return -1;
        }
    }

    confParam.nVmType = PVT_CT;
    confParam.sConfigSample = "vswap.1024MB";
    confParam.nOsVersion = 0;

    job = PrlSrv_GetDefaultVmConfig(privconn->server, &confParam, 0);
    if (PRL_FAILED(getJobResult(job, &result)))
        goto cleanup;

    pret = PrlResult_GetParamByIndex(result, 0, &sdkdom);
    prlsdkCheckRetGoto(pret, cleanup);

    if (useTemplate) {
        pret = PrlVmCfg_SetOsTemplate(sdkdom, def->fss[0]->src);
        prlsdkCheckRetGoto(pret, cleanup);

    }

    ret = prlsdkDoApplyConfig(conn, sdkdom, def, NULL);
    if (ret)
        goto cleanup;

    job = PrlVm_RegEx(sdkdom, "",
                      PACF_NON_INTERACTIVE_MODE | PRNVM_PRESERVE_DISK);
    if (PRL_FAILED(waitJob(job)))
        ret = -1;

 cleanup:
    PrlHandle_Free(sdkdom);
    return ret;
}

int
prlsdkUnregisterDomain(parallelsConnPtr privconn, virDomainObjPtr dom)
{
    parallelsDomObjPtr privdom = dom->privateData;
    PRL_HANDLE job;
    size_t i;

    for (i = 0; i < dom->def->nnets; i++)
       prlsdkDelNet(privconn, dom->def->nets[i]);

    job = PrlVm_Unreg(privdom->sdkdom);
    if (PRL_FAILED(waitJob(job)))
        return -1;

    if (prlsdkSendEvent(privconn, dom, VIR_DOMAIN_EVENT_UNDEFINED,
                        VIR_DOMAIN_EVENT_UNDEFINED_REMOVED) < 0)
        return -1;

    virDomainObjListRemove(privconn->domains, dom);
    return 0;
}

int
prlsdkDomainManagedSaveRemove(virDomainObjPtr dom)
{
    parallelsDomObjPtr privdom = dom->privateData;
    PRL_HANDLE job;

    job = PrlVm_DropSuspendedState(privdom->sdkdom);
    if (PRL_FAILED(waitJob(job)))
        return -1;

    return 0;
}
