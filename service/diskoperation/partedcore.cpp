/**
 * @copyright 2020-2020 Uniontech Technology Co., Ltd.
 *
 * @file partedcore.cpp
 *
 * @brief 磁盘操作类
 *
 * @date 2020-09-03 17:49
 *
 * Author: liweigang  <liweigang@uniontech.com>
 *
 * Maintainer: liweigang  <liweigang@uniontech.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "gpt_header.h"
#include "partedcore.h"
#include "fsinfo.h"
#include "mountinfo.h"
#include "partition.h"
#include "procpartitionsinfo.h"
#include "filesystems/filesystem.h"
#include "lvmoperator/lvmoperator.h"
#include "luksoperator/luksoperator.h"

#include <QDebug>
#include <linux/hdreg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>
#include <errno.h>
#include <string.h>
#include <set>
#include <tuple>



namespace DiskManager {
//hdparm可检测，显示与设定IDE或SCSI硬盘的参数。
//udevadm可检测设备热插拔
//static bool udevadm_found = false;
static bool hdparmFound = false;
const std::time_t SETTLE_DEVICE_PROBE_MAX_WAIT_SECONDS = 1;
//const std::time_t SETTLE_DEVICE_APPLY_MAX_WAIT_SECONDS = 10;
SupportedFileSystems *PartedCore::m_supportedFileSystems = nullptr;

PartedCore::PartedCore(QObject *parent)
    : QObject(parent), m_isClear(false)
{
    initConnection();
    qDebug() << __FUNCTION__ << "^^1";

    for (PedPartitionFlag flag = ped_partition_flag_next(static_cast<PedPartitionFlag>(NULL));
            flag; flag = ped_partition_flag_next(flag))
        m_flags.push_back(flag);

    qDebug() << __FUNCTION__ << "^^2";

    findSupportedCore();

    qDebug() << __FUNCTION__ << "^^3";

    m_supportedFileSystems = new SupportedFileSystems();
    //Determine file system support capabilities for the first time
    m_supportedFileSystems->findSupportedFilesystems();

    qDebug() << __FUNCTION__ << "^^4";

    m_workerThreadProbe = nullptr;
    m_workerCheckThread = nullptr;
    m_workerFixThread = nullptr;

    m_workerLVMThread = nullptr;
    probeDeviceInfo();
    delTempMountFile();


    qDebug() << __FUNCTION__ << "^^5";
}

PartedCore::~PartedCore()
{
    delete m_supportedFileSystems;
    m_supportedFileSystems = nullptr;


    if (m_workerCheckThread) {
        m_workerCheckThread->quit();
        m_workerCheckThread->wait();
        delete m_workerCheckThread;
        m_workerCheckThread = nullptr;
    }

    if (m_workerFixThread) {
        m_workerFixThread->quit();
        m_workerFixThread->wait();
        delete m_workerFixThread;
        m_workerFixThread = nullptr;
    }

    if (m_workerThreadProbe) {
        delete m_workerThreadProbe;
        m_workerThreadProbe = nullptr;
    }


    if (m_workerLVMThread) {
        m_workerLVMThread->quit();
        m_workerLVMThread->wait();
        delete m_workerLVMThread;
        m_workerLVMThread = nullptr;
    }
    delTempMountFile();
}

void PartedCore::initConnection()
{
    connect(this, &PartedCore::refreshDeviceInfo, this, &PartedCore::onRefreshDeviceInfo);
    connect(&m_probeThread, &ProbeThread::updateDeviceInfo, this, &PartedCore::syncDeviceInfo);
    connect(&m_probeThread, &ProbeThread::unmountPartition, this, &PartedCore::unmountPartition);
    connect(&m_probeThread, &ProbeThread::deletePartitionMessage, this, &PartedCore::deletePartitionMessage);
    connect(&m_probeThread, &ProbeThread::showPartitionInfo, this, &PartedCore::showPartitionInfo);
    connect(&m_probeThread, &ProbeThread::createTableMessage, this, &PartedCore::createTableMessage);
    connect(&m_probeThread, &ProbeThread::usbUpdated, this, &PartedCore::usbUpdated);
    connect(&m_probeThread, &ProbeThread::clearPartitionMessage, this, &PartedCore::clearMessage);
    connect(&m_probeThread, &ProbeThread::vgCreateMessage, this, &PartedCore::vgCreateMessage);
    connect(&m_probeThread, &ProbeThread::pvDeleteMessage, this, &PartedCore::pvDeleteMessage);
    connect(&m_probeThread, &ProbeThread::vgDeleteMessage, this, &PartedCore::vgDeleteMessage);
    connect(&m_probeThread, &ProbeThread::lvDeleteMessage, this, &PartedCore::lvDeleteMessage);
    connect(this, &PartedCore::probeAllInfo, &m_probeThread, &ProbeThread::probeDeviceInfo);

    //connect(&m_probeThread, &ProbeThread::updateDeviceInfo, this, &PartedCore::updateDeviceInfo);
    connect(&m_checkThread, &WorkThread::checkBadBlocksInfo, this, &PartedCore::checkBadBlocksCountInfo);
    connect(&m_checkThread, &WorkThread::checkBadBlocksFinished, this, &PartedCore::checkBadBlocksFinished);
    connect(&m_fixthread, &FixThread::fixBadBlocksInfo, this, &PartedCore::fixBadBlocksInfo);
    connect(&m_fixthread, &FixThread::fixBadBlocksFinished, this, &PartedCore::fixBadBlocksFinished);


    connect(this, &PartedCore::checkBadBlocksRunCountStart, &m_checkThread, &WorkThread::runCount);
    connect(this, &PartedCore::checkBadBlocksRunTimeStart, &m_checkThread, &WorkThread::runTime);
    connect(this, &PartedCore::fixBadBlocksStart, &m_fixthread, &FixThread::runFix);

    connect(this, &PartedCore::deletePVListStart, &m_lvmThread, &LVMThread::deletePVList);
    connect(this, &PartedCore::resizeVGStart, &m_lvmThread, &LVMThread::resizeVG);

    connect(&m_lvmThread, &LVMThread::deletePVListFinished, this, &PartedCore::deletePVListMessage);
    connect(&m_lvmThread, &LVMThread::resizeVGFinished, this, &PartedCore::resizeVGMessage);

}

void PartedCore::findSupportedCore()
{
    //udevadm_found = !Utils::findProgramInPath("udevadm").isEmpty();
    hdparmFound = !Utils::findProgramInPath("hdparm").isEmpty();
}

bool PartedCore::supportedFileSystem(FSType fstype)
{
    if (nullptr == m_supportedFileSystems) {
        m_supportedFileSystems = new SupportedFileSystems();
        //Determine file system support capabilities for the first time
        m_supportedFileSystems->findSupportedFilesystems();
    }
    return m_supportedFileSystems->getFsObject(fstype) != nullptr;
}

const FS &PartedCore::getFileSystem(FSType fstype) const
{
    if (nullptr == m_supportedFileSystems) {
        m_supportedFileSystems = new SupportedFileSystems();
        //Determine file system support capabilities for the first time
        m_supportedFileSystems->findSupportedFilesystems();
    }
    return m_supportedFileSystems->getFsSupport(fstype);
}

FileSystem *PartedCore::getFileSystemObject(FSType fstype)
{
    if (nullptr == m_supportedFileSystems) {
        m_supportedFileSystems = new SupportedFileSystems();
        //Determine file system support capabilities for the first time
        m_supportedFileSystems->findSupportedFilesystems();
    }
    return m_supportedFileSystems->getFsObject(fstype);
}

//bool PartedCore::filesystem_resize_disallowed(const Partition &partition)
//{
//    if (partition.fstype == FS_LVM2_PV) {
//        //        //The LVM2 PV can't be resized when it's a member of an export VG
//        //        QString vgname = LVM2_PV_Info::get_vg_name(partition.getPath());
//        //        if (vgname .isEmpty())
//        //            return false ;
//        //        return LVM2_PV_Info::is_vg_exported(vgname);
//    }
//    return false;
//}

void PartedCore::insertUnallocated(const QString &devicePath, QVector<Partition *> &partitions, Sector start, Sector end, Byte_Value sectorSize, bool insideExtended)
{
    //if there are no partitions at all..
    if (partitions.empty()) {
        Partition *partitionTemp = new Partition();
        partitionTemp->setUnallocated(devicePath, start, end, sectorSize, insideExtended);
        partitions.push_back(partitionTemp);
        return;
    }

    //start <---> first partition start
    if ((partitions.front()->m_sectorStart - start) > (MEBIBYTE / sectorSize)) {
        Sector tempEnd = partitions.front()->m_sectorStart - 1;
        Partition *partitionTemp = new Partition();
        partitionTemp->setUnallocated(devicePath, start, tempEnd, sectorSize, insideExtended);
        partitions.insert(partitions.begin(), partitionTemp);
    }

    //look for gaps in between
    for (int t = 0; t < partitions.size() - 1; t++) {
        if (((partitions.at(t + 1)->m_sectorStart - partitions.at(t)->m_sectorEnd - 1) > (MEBIBYTE / sectorSize))
                || ((partitions.at(t + 1)->m_type != TYPE_LOGICAL) // Only show exactly 1 MiB if following partition is not logical.
                    && ((partitions.at(t + 1)->m_sectorStart - partitions.at(t)->m_sectorEnd - 1) == (MEBIBYTE / sectorSize)))) {
            Sector tempStart = partitions.at(t)->m_sectorEnd + 1;
            Sector tempEnd = partitions.at(t + 1)->m_sectorStart - 1;
            Partition *partitionTemp = new Partition();
            partitionTemp->setUnallocated(devicePath, tempStart, tempEnd,
                                          sectorSize, insideExtended);
            partitions.insert(partitions.begin() + (++t), partitionTemp);
            // partitions.insert_adopt(partitions.begin() + ++t, partition_temp);
        }
    }
//    partitions.back();
    //last partition end <---> end
    if ((end - partitions.back()->m_sectorEnd) >= (MEBIBYTE / sectorSize)) {
        Sector tempStart = partitions.back()->m_sectorEnd + 1;
        Partition *partitionTemp = new Partition();
        partitionTemp->setUnallocated(devicePath, tempStart, end, sectorSize, insideExtended);
        partitions.push_back(partitionTemp);
    }
}

void PartedCore::setFlags(Partition &partition, PedPartition *lpPartition)
{
    for (int t = 0; t < m_flags.size(); t++) {
        if (ped_partition_is_flag_available(lpPartition, m_flags[t]) && ped_partition_get_flag(lpPartition, m_flags[t]))
            partition.m_flags.push_back(ped_partition_flag_get_name(m_flags[t]));
    }
}

FS_Limits PartedCore::getFileSystemLimits(FSType fstype, const Partition &partition)
{
    if (nullptr == m_supportedFileSystems) {
        m_supportedFileSystems = new SupportedFileSystems();
        //Determine file system support capabilities for the first time
        m_supportedFileSystems->findSupportedFilesystems();
    }
    FileSystem *pFileSystem = m_supportedFileSystems->getFsObject(fstype);
    FS_Limits fsLimits;
    if (pFileSystem != nullptr)
        fsLimits = pFileSystem->getFilesystemLimits(partition);
    return fsLimits;
}

bool PartedCore::secuClear(const QString &path, const Sector &start, const Sector &end, const Byte_Value &size, const QString &fstype, const QString &name, const int &count)
{
    QString cmd, output, error;
    int exitCode = 0;
    long long allSecSize = (end - start + 1) * size;
    long long tmpSize  = allSecSize % (MEBIBYTE * 512);
    long long tempEnd = (end - start + 1) * size / (MEBIBYTE * 512); //磁盘中存在多少个512M
    struct stat fileStat;


    //清除末尾残留
    for (int i = 0; i < count; ++i) {
        int j = 0;
        while (j < tempEnd) {
            //判断是否为块设备
            stat(path.toStdString().c_str(), &fileStat);
            if (!S_ISBLK(fileStat.st_mode)) {
                qDebug() << __FUNCTION__ << QString("%1 file not exit").arg(path);
                return false;
            }
            cmd = QString("dd if=/dev/zero of=%1 bs=512M count=1 seek=%2 conv=nocreat").arg(path).arg(j);
            exitCode = Utils::executCmd(cmd, output, error);
            if (exitCode != 0) {
                qDebug() << __FUNCTION__ << QString("errorCode: %1,   error: %2 ").arg(exitCode).arg(error);
                return false;
            }

            j++;

            if (j >= tempEnd) {
                break;
            }

            qDebug() << __FUNCTION__ << QString("count size:%1M      current size:%2M").arg(end - 1 * size / 1024 / 1024).arg(j * MEBIBYTE * 512);
            qDebug() << __FUNCTION__ << QString("count num:%1       current num:%2").arg(tempEnd).arg(j);

            cmd = QString("dd if=/dev/urandom of=%1 bs=512M count=1 seek=%2 conv=nocreat").arg(path).arg(j);
            exitCode = Utils::executCmd(cmd, output, error);

            if (exitCode != 0 && j < tempEnd) {
                qDebug() << __FUNCTION__ << QString("errorCode: %1,   error: %2 ").arg(exitCode).arg(error);
                return false;
            }
            j++;
            if (j >= tempEnd) {
                break;
            }

            qDebug() << __FUNCTION__ << QString("count size:%1M      current size:%2M").arg(end - 1 * size / 1024 / 1024).arg(j * MEBIBYTE * 512);
            qDebug() << __FUNCTION__ << QString("count num:%1       current num:%2").arg(tempEnd).arg(j);
        }

        //清除末尾残留
        if (tmpSize > 0) {
            stat(path.toStdString().c_str(), &fileStat);
            if (!S_ISBLK(fileStat.st_mode)) {
                qDebug() << __FUNCTION__ << QString("%1 file not exit").arg(path);
                return false;
            }

            cmd = QString("dd if=/dev/zero of=%1 bs=%2 count=1 seek=%3 conv=nocreat oflag=seek_bytes").arg(path).arg(tmpSize).arg(j * (MEBIBYTE * 512));
            exitCode = Utils::executCmd(cmd, output, error);
            if (exitCode != 0) {
                qDebug() << __FUNCTION__ << QString("errorCode: %1,   error: ").arg(exitCode).arg(error);
                return false;
            }
        }

        qDebug() << __FUNCTION__ << " secuClear end ";
        qDebug() << __FUNCTION__ << QString("count loop:%1      current loop:%2 ").arg(count).arg(i);
    }
    return true;
}

void PartedCore::probeDeviceInfo(const QString &)
{
    m_inforesult.clear();
    m_deviceMap.clear();
    QString rootFsName;
    QVector<QString> devicePaths;
    //qDebug() << __FUNCTION__ << "**1";
    devicePaths.clear();
    BlockSpecial::clearCache();
    //qDebug() << __FUNCTION__ << "**2";
    ProcPartitionsInfo::loadCache();
    //qDebug() << __FUNCTION__ << "**3";
    FsInfo::loadCache();
    //qDebug() << __FUNCTION__ << "**4";
    //qDebug() << __FUNCTION__ << "**5";
    MountInfo::loadCache(rootFsName);
    //qDebug() << __FUNCTION__ << "**6";
    ped_device_probe_all();
    //qDebug() << __FUNCTION__ << "**7";
    PedDevice *lpDevice = ped_device_get_next(nullptr);
    while (lpDevice) {
        /* TO TRANSLATORS: looks like   Confirming /dev/sda */
        qDebug() << QString("Confirming %1").arg(lpDevice->path);

        //only add this device if we can read the first sector (which means it's a real device)
        if (useableDevice(lpDevice))
            devicePaths.push_back(lpDevice->path);
//        qDebug() << lpDevice->path;
        lpDevice = ped_device_get_next(lpDevice);
    }
//    qDebug() << __FUNCTION__ << "devicepaths size=" << devicepaths.size();
    std::sort(devicePaths.begin(), devicePaths.end());
    //qDebug() << __FUNCTION__ << "**8";
    for (int t = 0; t < devicePaths.size(); t++) {
        /*TO TRANSLATORS: looks like Searching /dev/sda partitions */
        Device tempDevice;
        setDeviceFromDisk(tempDevice, devicePaths[t]);
        DeviceStorage device;
        tempDevice.m_mediaType = device.getDiskInfoMediaType(devicePaths[t]);
        device.getDiskInfoModel(devicePaths[t], tempDevice.m_model);
        device.getDiskInfoInterface(devicePaths[t], tempDevice.m_interface, tempDevice.m_model);

        m_deviceMap.insert(devicePaths.at(t), tempDevice);
    }
    //qDebug() << __FUNCTION__ << "**9";
//    getPartitionHiddenFlag();
    for (auto it = m_deviceMap.begin(); it != m_deviceMap.end(); it++) {
        DeviceInfo devinfo = it.value().getDeviceInfo();
        for (int i = 0; i < it.value().m_partitions.size(); i++) {
            const Partition &pat = *(it.value().m_partitions.at(i)); //拷贝构造速度提升 const 引用
            PartitionInfo partinfo = pat.getPartitionInfo();

//            if(m_hiddenPartition.indexOf(partinfo.m_uuid) != -1 && !partinfo.m_uuid.isEmpty()) {
//                partinfo.m_flag = 1;
//            } else {
//                partinfo.m_flag = 0;
//            }


            //partinfo = pat.getPartitionInfo();
            if (rootFsName == pat.getPath()) {
                partinfo.m_flag = 4;
                qDebug() << __FUNCTION__ << "Set systemfs Flags 1 !! " << pat.m_devicePath << " " << pat.m_name << " " << pat.m_uuid;
            }

            qDebug() << __FUNCTION__ << " EXTEND is " << PartitionType::TYPE_EXTENDED << " and I am " << pat.m_type;
            if (pat.m_type == PartitionType::TYPE_EXTENDED) {
                devinfo.m_partition.push_back(partinfo);
                for (int k = 0; k < pat.m_logicals.size(); k++) {
                    const Partition &plogic = *(pat.m_logicals.at(k));
                    partinfo = plogic.getPartitionInfo();
                    if (rootFsName == plogic.m_name) {
                        partinfo.m_flag = 4;
                        qDebug() << __FUNCTION__ << "Set systemfs Flags2 !! " << plogic.m_devicePath << " " << plogic.m_name << " " << plogic.m_uuid;
                    }
                    qDebug() << __FUNCTION__ << plogic.m_devicePath << " " << plogic.m_name << " " << plogic.m_uuid;
                    devinfo.m_partition.push_back(partinfo);
                }
            } else {
                devinfo.m_partition.push_back(partinfo);
            }
        }
        m_inforesult.insert(devinfo.m_path, devinfo);
    }
    LVMOperator::getDeviceDataAndLVMInfo(m_inforesult, m_lvmInfo);
    LUKSOperator::updateLUKSInfo(m_inforesult, m_lvmInfo, m_LUKSInfo);
//    qDebug() << __FUNCTION__ << m_inforesult.count();
    //qDebug() << __FUNCTION__ << "**10";
}

bool PartedCore::useableDevice(const PedDevice *lpDevice)
{
    Q_ASSERT(nullptr != lpDevice);
    char *buf = static_cast<char *>(malloc(lpDevice->sector_size));
    if (!buf)
        return false;

    // Must be able to read from the first sector before the disk device is considered
    // useable in GParted.
    bool success = false;
    int fd = open(lpDevice->path, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
        ssize_t bytesRead = read(fd, buf, lpDevice->sector_size);
        success = (bytesRead == lpDevice->sector_size);
        close(fd);
    }
    free(buf);
    return success;
}

void PartedCore::setDeviceFromDisk(Device &device, const QString &devicePath)
{
    PedDevice *lpDevice = nullptr;
    PedDisk *lpDisk = nullptr;
    if (getDevice(devicePath, lpDevice, true)) {
        device.m_heads = lpDevice->bios_geom.heads;
        device.m_length = lpDevice->length;
        device.m_path = devicePath;
        device.m_model = lpDevice->model;
        device.m_sectorSize = lpDevice->sector_size;
        device.m_sectors = lpDevice->bios_geom.sectors;
        device.m_cylinders = lpDevice->bios_geom.cylinders;
        device.m_cylsize = device.m_heads * device.m_sectors;
        setDeviceSerialNumber(device);
        if (device.m_cylsize < (MEBIBYTE / device.m_sectorSize))
            device.m_cylsize = MEBIBYTE / device.m_sectorSize;

        FSType fstype = detectFilesystem(lpDevice, nullptr);
        if (fstype != FSType::FS_UNKNOWN) {
            device.m_diskType = "none";
            device.m_maxPrims = 1;
            setDeviceOnePartition(device, lpDevice, fstype);
        } else if (getDisk(lpDevice, lpDisk, false)) {
            // Partitioned drive (excluding "loop"), as recognised by libparted
            if (lpDisk && lpDisk->type && lpDisk->type->name && strcmp(lpDisk->type->name, "loop") != 0) {
                device.m_diskType = lpDisk->type->name;
                device.m_maxPrims = ped_disk_get_max_primary_partition_count(lpDisk);

                // Determine if partition naming is supported.
                if (ped_disk_type_check_feature(lpDisk->type, PED_DISK_TYPE_PARTITION_NAME)) {
                    device.enablePartitionNaming(Utils::getMaxPartitionNameLength(device.m_diskType));
                }

                setDevicePartitions(device, lpDevice, lpDisk);

                if (device.m_highestBusy) {
                    device.m_readonly = !commitToOs(lpDisk);
                }
            }
            // Drive just containing libparted "loop" signature and nothing
            // else.  (Actually any drive reported by libparted as "loop" but
            // not recognised by blkid on the whole disk device).
            else if (lpDisk && lpDisk->type && strcmp(lpDisk->type->name, "loop") == 0) {
//            else if (lpDisk && lpDisk->type && lpDisk->type->name && strcmp(lpDisk->type->name, "loop") == 0) {
                device.m_diskType = lpDisk->type->name; //赋值
                device.m_maxPrims = 1; //赋值

                // Create virtual partition covering the whole disk device
                // with unknown contents.
                Partition *partition_temp = new Partition();
                partition_temp->setUnpartitioned(device.m_path,
                                                 lpDevice->path,
                                                 FS_UNKNOWN,
                                                 device.m_length,
                                                 device.m_sectorSize,
                                                 false);
                // Place unknown file system message in this partition.
                device.m_partitions.push_back(partition_temp);
            }
            // Unrecognised, unpartitioned drive.
            else {
                device.m_diskType = "unrecognized";
                device.m_maxPrims = 1;

                Partition *partition_temp = new Partition();
                partition_temp->setUnpartitioned(device.m_path,
                                                 "", // Overridden with "unallocated"
                                                 FS_UNALLOCATED,
                                                 device.m_length,
                                                 device.m_sectorSize,
                                                 false);
                device.m_partitions.push_back(partition_temp);
            }
        }
        destroyDeviceAndDisk(lpDevice, lpDisk);
    }
}

bool PartedCore::getDevice(const QString &devicePath, PedDevice *&lpDevice, bool flush)
{
    lpDevice = ped_device_get(devicePath.toStdString().c_str());


    int fd = open(devicePath.toStdString().c_str(), O_RDONLY);
    struct hd_geo {
        unsigned char heads;
        unsigned char sectors;
        unsigned short cylinders;
        unsigned long start;
    } geo;

    int ret = ioctl(fd, HDIO_GETGEO, &geo);
    if (0 == ret && geo.sectors > lpDevice->bios_geom.sectors) {
        lpDevice->bios_geom.sectors = geo.sectors;
        lpDevice->bios_geom.cylinders = lpDevice->length / (geo.heads * geo.sectors);
    }
    close(fd);

    if (lpDevice) {
        if (flush)
            // Force cache coherency before going on to read the partition
            // table so that libparted reading the whole disk device and the
            // file system tools reading the partition devices read the same
            // data.
            flushDevice(lpDevice);
        return true;
    }
    return false;
}

bool PartedCore::getDisk(PedDevice *&lpDevice, PedDisk *&lpDisk, bool strict)
{
    if (lpDevice) {
        lpDisk = ped_disk_new(lpDevice);

        // (#762941)(!46) After ped_disk_new() wait for triggered udev rules to
        // to complete which remove and re-add all the partition specific /dev
        // entries to avoid FS specific commands failing because they happen to
        // be running when the needed /dev/PTN entries don't exist.
        //settleDevice(SETTLE_DEVICE_PROBE_MAX_WAIT_SECONDS);
        QString out, err;
        Utils::executCmd(QString("udevadm settle --timeout=%1").arg(SETTLE_DEVICE_PROBE_MAX_WAIT_SECONDS), out, err);
        // if ! disk and writable it's probably a HD without disklabel.
        // We return true here and deal with them in
        // GParted_Core::setDeviceFromDisk().
        if (lpDisk || (!strict && !lpDevice->read_only))
            return true;

        destroyDeviceAndDisk(lpDevice, lpDisk);
    }

    return false;
}

void PartedCore::destroyDeviceAndDisk(PedDevice *&lpDevice, PedDisk *&lpDisk)
{
    if (lpDisk)
        ped_disk_destroy(lpDisk);
    lpDisk = nullptr;

    if (lpDevice)
        ped_device_destroy(lpDevice);
    lpDevice = nullptr;
}

bool PartedCore::infoBelongToPartition(const Partition &partition, const PartitionInfo &info)
{
    return info.m_sectorEnd == partition.m_sectorEnd && info.m_sectorStart == partition.m_sectorStart;
}

bool PartedCore::getDeviceAndDisk(const QString &devicePath, PedDevice *&lpDevice, PedDisk *&lpDisk, bool strict, bool flush)
{
    if (getDevice(devicePath, lpDevice, flush)) {
        return getDisk(lpDevice, lpDisk, strict);
    }

    return false;
}

bool PartedCore::commit(PedDisk *lpDisk)
{
    bool opened = ped_device_open(lpDisk->dev);

    bool succeed = ped_disk_commit_to_dev(lpDisk);

    succeed = commitToOs(lpDisk) && succeed;

    if (opened) {
        ped_device_close(lpDisk->dev);
        // Wait for udev rules to complete and partition device nodes to settle
        // from this ped_device_close().
        //settleDevice(SETTLE_DEVICE_PROBE_MAX_WAIT_SECONDS);
        QString out, err;
        Utils::executCmd(QString("udevadm settle --timeout=%1").arg(SETTLE_DEVICE_PROBE_MAX_WAIT_SECONDS), out, err);
    }

    return succeed;
}

PedPartition *PartedCore::getLpPartition(const PedDisk *lpDisk, const Partition &partition)
{
    if (partition.m_type == PartitionType::TYPE_EXTENDED)
        return ped_disk_extended_partition(lpDisk);
    return ped_disk_get_partition_by_sector(lpDisk, partition.getSector());
}

void PartedCore::setDeviceSerialNumber(Device &device)
{
    if (!hdparmFound)
        // Serial number left blank when the hdparm command is not installed.
        return;

    QString output, error;
    Utils::executCmd(QString("hdparm -I %1").arg(device.m_path), output, error);
    if (error.isEmpty()) {
        // hdparm reported an error message to stderr.  Assume it's a device
        // without a hard drive serial number.
        //
        // Using hdparm -I to query Linux software RAID arrays and BIOS fake RAID
        // arrays, both devices without their own hard drive serial numbers,
        // produce this error:
        //     HDIO_DRIVE_CMD(identify) failed: Inappropriate ioctl for device
        //
        // And querying USB flash drives, also a device type without their own
        // hard drive serial numbers, generates this error:
        //     SG_IO: bad/missing sense data, sb[]:  70 00 05 00 00 00 00 0a ...
        device.m_serialNumber = "none";
    } else {
        QString serialNumber = Utils::regexpLabel(output, "(?<=Serial Number:).*(?=\n)").trimmed();
        if (!serialNumber.isEmpty())
            device.m_serialNumber = serialNumber;
    }
    // Otherwise serial number left blank when not found in the hdparm output.
}

void PartedCore::setDeviceOnePartition(Device &device, PedDevice *lpDevice, FSType fstype)
{
    device.m_partitions.clear();
    QString path(lpDevice->path);
    bool partitionIsBusy = isBusy(fstype, path);

    Partition *partitionTemp = nullptr;
    if (fstype == FSType::FS_LUKS) {
        partitionTemp = nullptr; //= new PartitionLUKS();
        return;
    } else
        partitionTemp = new Partition();
//    if (nullptr == partitionTemp)
//        return;
    partitionTemp->setUnpartitioned(device.m_path,
                                    path,
                                    fstype,
                                    device.m_length,
                                    device.m_sectorSize,
                                    partitionIsBusy);

    //        if ( fstype == FS_LUKS )
    //            set_luks_partition( *dynamic_cast<PartitionLUKS *>( partition_temp ) );

    if (partitionTemp->m_busy)
        device.m_highestBusy = 1;

    setPartitionLabelAndUuid(*partitionTemp);
    setMountPoints(*partitionTemp);
    setUsedSectors(*partitionTemp, nullptr);
    device.m_partitions.push_back(partitionTemp);
}

void PartedCore::setPartitionLabelAndUuid(Partition &partition)
{
    QString partitionPath = partition.getPath();
    readLabel(partition);
    if (!partition.filesystemLabelKnown()) {
        bool labelFound = false;
        QString label = FsInfo::getLabel(partitionPath, labelFound);
        if (labelFound)
            partition.setFilesystemLabel(label);
    }

    // Retrieve file system UUID.  Use cached method first in an effort to speed up
    // device scanning.
    partition.m_uuid = FsInfo::getUuid(partitionPath);
    partition.m_name = partitionPath;
    if (partition.m_uuid.isEmpty()) {
        readUuid(partition);
    }
}

bool PartedCore::isBusy(FSType fstype, const QString &path, const PedPartition *lpPartition)
{
    FileSystem *pFilesystem = nullptr;
    bool busy = false;
    if (nullptr != lpPartition) {
        busy = ped_partition_is_busy(lpPartition);
    }
    if (!busy && supportedFileSystem(fstype)) {
        switch (getFileSystem(fstype).busy) {
        case FS::GPARTED:
            //Search GParted internal mounted partitions map
            busy = MountInfo::isDevMounted(path);
            break;
        case FS::EXTERNAL:
            //Call file system specific method
            pFilesystem = getFileSystemObject(fstype);
            if (pFilesystem)
                busy = pFilesystem->isBusy(path);
            break;

        default:
            break;
        }
    }

    return busy;
}

void PartedCore::readLabel(Partition &partition)
{
    FileSystem *pFilesystem = nullptr;
    switch (getFileSystem(partition.m_fstype).read_label) {
    case FS::EXTERNAL:
        pFilesystem = getFileSystemObject(partition.m_fstype);
        if (pFilesystem)
            pFilesystem->readLabel(partition);
        break;

    default:
        break;
    }
}

void PartedCore::readUuid(Partition &partition)
{
    FileSystem *pFilesystem = nullptr;
    switch (getFileSystem(partition.m_fstype).read_uuid) {
    case FS::EXTERNAL:
        pFilesystem = getFileSystemObject(partition.m_fstype);
        if (pFilesystem)
            pFilesystem->readUuid(partition);
        break;

    default:
        break;
    }
}

void PartedCore::setMountPoints(Partition &partition)
{
    //DMRaid dmraid ; //Use cache of dmraid device information
    if (partition.m_fstype == FSType::FS_LVM2_PV) {
        //        QString vgname = LVM2_PV_Info::get_vg_name(partition.getPath());
        //        if (! vgname.isEmpty())
        //            partition.addMountPoint(vgname);
    } else if (partition.m_fstype == FSType::FS_LINUX_SWRAID) {
        //        QString array_path = SWRaid_Info::get_array(partition.getPath());
        //        if (! array_path.isEmpty())
        //            partition.addMountPoint(array_path);
    } else if (partition.m_fstype == FSType::FS_ATARAID) {
        //        QString array_path = SWRaid_Info::get_array(partition.getPath());
        //        if (! array_path.isEmpty()) {
        //            partition.addMountPoint(array_path);
        //        } else {
        //            array_path = dmraid.get_array(partition.getPath());
        //            if (! array_path.isEmpty())
        //                partition.addMountPoint(array_path);
        //        }
    } else if (partition.m_fstype == FSType::FS_LUKS) {
        //        LUKS_Mapping mapping = LUKS_Info::get_cache_entry(partition.getPath());
        //        if (! mapping.name.isEmpty())
        //            partition.addMountPoint(DEV_MAPPER_PATH + mapping.name);
    }
    // Swap spaces don't have mount points so don't bother trying to add them.
    else if (partition.m_fstype != FSType::FS_LINUX_SWAP) {
        if (partition.m_busy) {
            // Normal device, not DMRaid device
            if (setMountPointsHelper(partition, partition.getPath()))
                return;

            qDebug() << __FUNCTION__ << "xxxUnable to find mount point";
        } else { // Not busy file system
            partition.addMountPoints(MountInfo::getFileSystemTableMountpoints(partition.getPath()));
        }
    }
}

bool PartedCore::setMountPointsHelper(Partition &partition, const QString &path)
{
    QString searchPath;
    if (partition.m_fstype == FSType::FS_BTRFS)
        searchPath = path; //btrfs::get_mount_device( path ) ;
    else
        searchPath = path;

    const QVector<QString> &mountpoints = MountInfo::getMountedMountpoints(searchPath);
    if (mountpoints.size()) {
        partition.addMountPoints(mountpoints);
        partition.m_fsReadonly = MountInfo::isDevMountedReadonly(searchPath);
        return true;
    }

    return false;
}

void PartedCore::setUsedSectors(Partition &partition, PedDisk *lpDisk)
{
    Q_UNUSED(lpDisk)
    if (supportedFileSystem(partition.m_fstype)) {
        FileSystem *pFilesystem = nullptr;
        if (partition.m_busy) {
            switch (getFileSystem(partition.m_fstype).online_read) {
            case FS::EXTERNAL:
                pFilesystem = getFileSystemObject(partition.m_fstype);
                if (pFilesystem)
                    pFilesystem->setUsedSectors(partition);
                break;
            case FS::GPARTED:
                mountedFileSystemSetUsedSectors(partition);
                break;
            default:
                break;
            }
        } else { // Not busy file system
            switch (getFileSystem(partition.m_fstype).read) {
            case FS::EXTERNAL:
                pFilesystem = getFileSystemObject(partition.m_fstype);
                if (pFilesystem)
                    pFilesystem->setUsedSectors(partition);
                break;
#ifdef HAVE_LIBPARTED_FS_RESIZE
            case FS::LIBPARTED:
                if (lp_disk)
                    LpSetUsedSectors(partition, lp_disk);
                break;
#endif
            default:
                break;
            }
        }

        Sector unallocated;
        // Only confirm that the above code succeeded in setting the sector usage
        // values for this base Partition object, hence the explicit call to the
        // base Partition class sectorUsageKnown() method.  For LUKS this avoids
        // calling derived PartitionLUKS class sectorUsageKnown() which also
        // checks for known sector usage in the encrypted file system.  But that
        // wasn't set by the above code so in the case of luks/unknown would
        // produce a false positive.
        if (!partition.sectorUsageKnown()) {
            if (!Utils::getFileSystemSoftWare(partition.m_fstype).isEmpty()) {
                QString msg("The following list of software packages is required for %1 file system support:  %2.");
                msg = msg.arg(Utils::fileSystemTypeToString(partition.m_fstype)).arg(Utils::getFileSystemSoftWare(partition.m_fstype));
                qDebug() << __FUNCTION__ << msg;
            }

        } else if ((unallocated = partition.getSectorsUnallocated()) > 0) {
            /* TO TRANSLATORS: looks like   1.28GiB of unallocated space within the partition. */
            QString temp("%1 of unallocated space within the partition.");
            temp = temp.arg(Utils::formatSize(unallocated, partition.m_sectorSize));
            FS fs = getFileSystem(partition.m_fstype);
            if (fs.check != FS::NONE && fs.grow != FS::NONE) {
                temp.append("To grow the file system to fill the partition, select the partition and choose the menu item:\n");
                temp.append("Partition --> Check.");
                qDebug() << __FUNCTION__ << temp;
            }
        }

//        if (filesystem_resize_disallowed(partition)) {
//            //            QString temp = getFileSystemObject(partition.fstype)
//            //                                 ->get_custom_text(CTEXT_RESIZE_DISALLOWED_WARNING);
//        }
    } else {
        // Set usage of mounted but unsupported file systems.
        if (partition.m_busy)
            mountedFileSystemSetUsedSectors(partition);
    }
}

void PartedCore::mountedFileSystemSetUsedSectors(Partition &partition)
{
    if (partition.getMountPoints().size() > 0 && MountInfo::isDevMounted(partition.getPath())) {
        Byte_Value fs_size;
        Byte_Value fs_free;
        if (Utils::getMountedFileSystemUsage(partition.getMountPoint(), fs_size, fs_free) == 0)
            partition.setSectorUsage(fs_size / partition.m_sectorSize,
                                     fs_free / partition.m_sectorSize);
    }
}

void PartedCore::setDevicePartitions(Device &device, PedDevice *lpDevice, PedDisk *lpDisk)
{
    int extindex = -1;
    device.m_partitions.clear();

    PedPartition *lpPartition = ped_disk_next_partition(lpDisk, nullptr);
    while (lpPartition) {
        Partition *partitionTemp = nullptr;
        bool partitionIsBusy = false;
        FSType fstype = FS_UNKNOWN;
        QString partitionPath;
        switch (lpPartition->type) {
        case PED_PARTITION_NORMAL:
        case PED_PARTITION_LOGICAL:
            fstype = detectFilesystem(lpDevice, lpPartition);
            partitionPath = getPartitionPath(lpPartition);
            partitionIsBusy = isBusy(fstype, partitionPath, lpPartition);
            qDebug() << partitionIsBusy << lpPartition->num << lpPartition->disk->dev->path;
            //            if (fstype == FS_LUKS)
            //                partition_temp = new PartitionLUKS();
            //            else
            partitionTemp = new Partition();
            partitionTemp->set(device.m_path,
                               partitionPath,
                               lpPartition->num,
                               (lpPartition->type == PED_PARTITION_NORMAL) ? TYPE_PRIMARY
                               : TYPE_LOGICAL,
                               fstype,
                               lpPartition->geom.start,
                               lpPartition->geom.end,
                               device.m_sectorSize,
                               (lpPartition->type == PED_PARTITION_LOGICAL),
                               partitionIsBusy);

            setFlags(*partitionTemp, lpPartition);

            //if (fstype == FS_LUKS)
            // set_luks_partition(*dynamic_cast<PartitionLUKS *>(partition_temp));

            if (partitionTemp->m_busy && partitionTemp->m_partitionNumber > device.m_highestBusy)
                device.m_highestBusy = partitionTemp->m_partitionNumber;
            break;

        case PED_PARTITION_EXTENDED:
            partitionPath = getPartitionPath(lpPartition);

            partitionTemp = new Partition();
            partitionTemp->set(device.m_path,
                               partitionPath,
                               lpPartition->num,
                               TYPE_EXTENDED,
                               FS_EXTENDED,
                               lpPartition->geom.start,
                               lpPartition->geom.end,
                               device.m_sectorSize,
                               false,
                               false);

            setFlags(*partitionTemp, lpPartition);

            extindex = device.m_partitions.size();
            break;

        default:
            qDebug() << ped_partition_is_busy(lpPartition) << lpPartition->num << lpPartition->disk->dev->path;
            // Ignore libparted reported partitions with other type
            // bits set.
            break;
        }

        // Only for libparted reported partition types that we care about: NORMAL,
        // LOGICAL, EXTENDED
        if (partitionTemp != nullptr) {
            setPartitionLabelAndUuid(*partitionTemp);
            setMountPoints(*partitionTemp);
            setUsedSectors(*partitionTemp, lpDisk);

            // Retrieve partition name
            if (device.partitionNamingSupported())
                partitionTemp->m_name = ped_partition_get_name(lpPartition);

            if (!partitionTemp->m_insideExtended)
                device.m_partitions.push_back(partitionTemp);
            else
                device.m_partitions[extindex]->m_logicals.push_back(partitionTemp);
        }

        //next partition (if any)
        lpPartition = ped_disk_next_partition(lpDisk, lpPartition);
    }

    if (extindex > -1) {
        insertUnallocated(device.m_path,
                          device.m_partitions.at(extindex)->m_logicals,
                          device.m_partitions.at(extindex)->m_sectorStart,
                          device.m_partitions.at(extindex)->m_sectorEnd,
                          device.m_sectorSize,
                          true);

        //Set busy status of extended partition if and only if
        //  there is at least one busy logical partition.
        for (int t = 0; t < device.m_partitions.at(extindex)->m_logicals.size(); t++) {
            if (device.m_partitions.at(extindex)->m_logicals.at(t)->m_busy) {
                device.m_partitions.at(extindex)->m_busy = true;
                break;
            }
        }
    }

    insertUnallocated(device.m_path, device.m_partitions, 0, device.m_length - 1, device.m_sectorSize, false);
}

bool PartedCore::flushDevice(PedDevice *lpDevice)
{
    bool success = false;
    if (ped_device_open(lpDevice)) {
        success = ped_device_sync(lpDevice);
        ped_device_close(lpDevice);
        // (!46) Wait for udev rules to complete after this ped_device_open() and
        // ped_device_close() pair to avoid busy /dev/DISK entry when running file
        // system specific querying commands on the whole disk device in the call
        // sequence after getDevice() in setDeviceFromDisk().
        //settleDevice(SETTLE_DEVICE_PROBE_MAX_WAIT_SECONDS);
        QString out, err;
        Utils::executCmd(QString("udevadm settle --timeout=%1").arg(SETTLE_DEVICE_PROBE_MAX_WAIT_SECONDS), out, err);
    }
    return success;
}

//void PartedCore::settleDevice(std::time_t timeout)
//{
//    //如果支持udevadm
//    //udevadm settle [options]　　查看udev事件队列，如果所有事件全部处理完就退出。timeout超时时间
//    if (udevadm_found) {
//        QString out, err;
//        Utils::executCmd(QString("udevadm settle --timeout=%1").arg(timeout), out, err);
//    } else
//        sleep(timeout);
//}

bool PartedCore::commitToOs(PedDisk *lpDisk)
{
//    bool succes;
//    succes = ped_disk_commit_to_os(lpDisk);
    // Wait for udev rules to complete and partition device nodes to settle from above
    // ped_disk_commit_to_os() initiated kernel update of the partitions.
    //settleDevice(timeout);
    return ped_disk_commit_to_os(lpDisk);
}

FSType PartedCore::detectFilesystem(PedDevice *lpDevice, PedPartition *lpPartition)
{
    QString fileSystemName;
    QString path;
    // Will query whole disk device using methods: (Q1) RAID, (Q2) blkid,
    // (Q4) internal
    if (lpPartition)
        path = getPartitionPath(lpPartition);
    else
        path = lpDevice->path;

    fileSystemName = FsInfo::getFileSystemType(path);
    FSType fsType = FSType::FS_UNKNOWN;
    if (fileSystemName.isEmpty() && lpPartition && lpPartition->fs_type)
        fileSystemName = lpPartition->fs_type->name;
    if (!fileSystemName.isEmpty()) {
        fsType = Utils::stringToFileSystemType(fileSystemName);
//        qDebug() << fstype;
        if (fsType != FSType::FS_UNKNOWN)
            return fsType;
    }

    fsType = detectFilesystemInternal(path, lpDevice->sector_size);
    if (fsType != FSType::FS_UNKNOWN)
        return fsType;

    //no file system found....
    QString temp("Unable to detect file system! Possible reasons are:\n- ");
    temp.append("The file system is damaged \n- ")
    .append("The file system is unknown to GParted \n-")
    .append("There is no file system available (unformatted) \n- ")
    .append(QString("The device entry %1 is missing").arg(path));
    qDebug() << __FUNCTION__ << temp;
    return FSType::FS_UNKNOWN;
}

FSType PartedCore::detectFilesystemInternal(const QString &path, Byte_Value sectorSize)
{
    char magic1[16]; // Big enough for largest signatures[].sig1 or sig2
    char magic2[16];
    FSType fsType = FSType::FS_UNKNOWN;

    char *buf = static_cast<char *>(malloc(sectorSize));
    if (!buf)
        return FSType::FS_UNKNOWN;

    int fd = open(path.toStdString().c_str(), O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        free(buf);
        return FSType::FS_UNKNOWN;
    }

    struct {
        Byte_Value offset1;
        const char *sig1;
        Byte_Value offset2;
        const char *sig2;
        FSType fstype;
    } signatures[] = {
        //offset1, sig1              , offset2, sig2  , fstype
        {0LL, "LUKS\xBA\xBE", 0LL, nullptr, FSType::FS_LUKS},
        {3LL, "-FVE-FS-", 0LL, nullptr, FSType::FS_BITLOCKER},
        {0LL, "\x52\x56\xBE\x1B", 0LL, nullptr, FSType::FS_GRUB2_CORE_IMG},
        {0LL, "\x52\x56\xBE\x6F", 0LL, nullptr, FSType::FS_GRUB2_CORE_IMG},
        {0LL, "\x52\xE8\x28\x01", 0LL, nullptr, FSType::FS_GRUB2_CORE_IMG},
        {0LL, "\x52\xBF\xF4\x81", 0LL, nullptr, FSType::FS_GRUB2_CORE_IMG},
        {0LL, "\x52\x56\xBE\x63", 0LL, nullptr, FSType::FS_GRUB2_CORE_IMG},
        {0LL, "\x52\x56\xBE\x56", 0LL, nullptr, FSType::FS_GRUB2_CORE_IMG},
        {24LL, "\x01\x00", 32LL, "NXSB", FSType::FS_APFS},
        {512LL, "LABELONE", 536LL, "LVM2", FSType::FS_LVM2_PV},
        {1030LL, "\x34\x34", 0LL, nullptr, FSType::FS_NILFS2},
        {65536LL, "ReIsEr4", 0LL, nullptr, FSType::FS_REISER4},
        {65600LL, "_BHRfS_M", 0LL, nullptr, FSType::FS_BTRFS}
    };
    // For simple BitLocker recognition consider validation of BIOS Parameter block
    // fields unnecessary.
    // *   Detecting BitLocker
    //     http://blogs.msdn.com/b/si_team/archive/2006/10/26/detecting-bitlocker.aspx
    //
    // Recognise GRUB2 core.img just by any of the possible first 4 bytes of x86 CPU
    // instructions it starts with.
    // *   bootinfoscript v0.77 line 1990  [GRUB2 core.img possible staring 4 bytes]
    //     https://github.com/arvidjaar/bootinfoscript/blob/009f509d59e2f0d39b8d44692e2a81720f5af7b6/bootinfoscript#L1990
    //
    // Simple APFS recognition based on matching the following fields in the
    // superblock:
    // 1)  Object type is OBJECT_TYPE_NX_SUPERBLOCK, lower 16-bits of the object type
    //     field is 0x0001 stored as little endian bytes 0x01, 0x00.
    //     WARNING: The magic signatures are defined as NUL terminated strings so the
    //     below code only does a 1-byte match for 0x01, rather than a 2-byte match
    //     for 0x01, 0x00.
    // 2)  4 byte magic "NXSB".
    // *   Apple File System Reference
    //     https://developer.apple.com/support/apple-file-system/Apple-File-System-Reference.pdf

    Byte_Value prevReadOffset = -1;
    memset(buf, 0, sectorSize);

    for (unsigned int i = 0; i < sizeof(signatures) / sizeof(signatures[0]); i++) {
        const size_t len1 = std::min((signatures[i].sig1 == nullptr) ? 0U : strlen(signatures[i].sig1),
                                     sizeof(magic1));
        const size_t len2 = std::min((signatures[i].sig2 == nullptr) ? 0U : strlen(signatures[i].sig2),
                                     sizeof(magic2));
        // NOTE: From this point onwards signatures[].sig1 and .sig2 are treated
        // as character buffers of known lengths len1 and len2, not NUL terminated
        // strings.
        if ((len1 == 0UL) || (signatures[i].sig2 != nullptr && len2 == 0UL))
            continue; // Don't allow 0 length signatures to match

        Byte_Value readOffset = signatures[i].offset1 / sectorSize * sectorSize;

        // Optimisation: only read new sector when it is different to the
        // previously read sector.
        if (readOffset != prevReadOffset) {
            if (lseek(fd, readOffset, SEEK_SET) == readOffset && read(fd, buf, sectorSize) == sectorSize) {
                prevReadOffset = readOffset;
            } else {
                // Outside block device boundaries or other error.
                continue;
            }
        }

        memcpy(magic1, buf + signatures[i].offset1 % sectorSize, len1);

        // WARNING: This assumes offset2 is in the same sector as offset1
        if (signatures[i].sig2 != nullptr)
            memcpy(magic2, buf + signatures[i].offset2 % sectorSize, len2);

        if (memcmp(magic1, signatures[i].sig1, len1) == 0 && (signatures[i].sig2 == nullptr || memcmp(magic2, signatures[i].sig2, len2) == 0)) {
            fsType = signatures[i].fstype;
            break;
        }
    }

    close(fd);
    free(buf);

    return fsType;
}

QString PartedCore::getPartitionPath(PedPartition *lpPartition)
{
    char *lpPath; //we have to free the result of ped_partition_get_path()
    QString partitionPath("Partition path not found");

    lpPath = ped_partition_get_path(lpPartition);
    if (lpPath != nullptr) {
        partitionPath = lpPath;
        free(lpPath);
    }
    return partitionPath;
}

void PartedCore::LpSetUsedSectors(Partition &partition, PedDisk *lpDisk)
{
    PedFileSystem *fs = nullptr;
    PedConstraint *constraint = nullptr;

    if (lpDisk) {
        PedPartition *lpPartition = ped_disk_get_partition_by_sector(lpDisk, partition.getSector());

        if (lpPartition) {
            fs = ped_file_system_open(&lpPartition->geom);

            if (fs) {
                constraint = ped_file_system_get_resize_constraint(fs);
                if (constraint) {
                    partition.setSectorUsage(fs->geom->length,
                                             fs->geom->length - constraint->min_size);

                    ped_constraint_destroy(constraint);
                }
                ped_file_system_close(fs);
            }
        }
    }
}

bool PartedCore::namePartition(const Partition &partition)
{
    QString msg;
    if (partition.m_name.isEmpty())
        msg = QString("Clear partition name on %1").arg(partition.getPath());
    else
        msg = QString("Set partition name to \"%1\" on %2").arg(partition.m_name).arg(partition.getPath());
    qDebug() << __FUNCTION__ << msg;

    bool success = false;
    PedDevice *lpDevice = nullptr;
    PedDisk *lpDisk = nullptr;
    if (getDeviceAndDisk(partition.m_devicePath, lpDevice, lpDisk)) {
        PedPartition *lpPartition = ped_disk_get_partition_by_sector(lpDisk, partition.getSector());
        if (lpPartition) {
            success = ped_partition_set_name(lpPartition, partition.m_name.toLatin1())
                      && commit(lpDisk);
        }
    }
    return success;
}

bool PartedCore::eraseFilesystemSignatures(const Partition &partition)
{
    if (partition.m_fstype == FS_LUKS && partition.m_busy) {
        qDebug() << __FUNCTION__ << "partition contains open LUKS encryption for an erase file system signatures only step";
        return false;
    }

    bool overallSuccess = false;
    qDebug() << __FUNCTION__ << QString("clear old file system signatures in %1").arg(partition.getPath());

    //Get device, disk & partition and open the device.  Allocate buffer and fill with
    //  zeros.  Buffer size is the greater of 4 KiB and the sector size.
    PedDevice *lpDevice = nullptr;
    PedDisk *lpDisk = nullptr;
    PedPartition *lpPartition = nullptr;
    bool deviceIsOpen = false;
    Byte_Value bufsize = 4LL * KIBIBYTE;
    char *buf = nullptr;
    if (getDevice(partition.m_devicePath, lpDevice)) {
        if (partition.m_type == TYPE_UNPARTITIONED) {
            // Virtual partition spanning whole disk device
            overallSuccess = true;
        } else if (getDisk(lpDevice, lpDisk)) {
            // Partitioned device
            lpPartition = ped_disk_get_partition_by_sector(lpDisk, partition.getSector());
            overallSuccess = (lpPartition != nullptr);
        }

        if (overallSuccess && ped_device_open(lpDevice)) {
            deviceIsOpen = true;

            bufsize = std::max(bufsize, lpDevice->sector_size);
            buf = static_cast<char *>(malloc(bufsize));
            if (buf)
                memset(buf, 0, bufsize);
        }
        overallSuccess &= deviceIsOpen;
    }
    struct {
        Byte_Value offset; //Negative offsets work backwards from the end of the partition
        Byte_Value rounding; //Minimum desired rounding for offset
        Byte_Value length;
    } ranges[] = {
        //offset           , rounding       , length
        {0LL, 1LL, 512LL * KIBIBYTE}, // All primary super blocks
        {64LL * MEBIBYTE, 1LL, 4LL * KIBIBYTE}, // Btrfs super block mirror copy
        {256LL * GIBIBYTE, 1LL, 4LL * KIBIBYTE}, // Btrfs super block mirror copy
        {1LL * PEBIBYTE, 1LL, 4LL * KIBIBYTE}, // Btrfs super block mirror copy
        {-512LL * KIBIBYTE, 256LL * KIBIBYTE, 512LL * KIBIBYTE}, // ZFS labels L2 and L3
        {-64LL * KIBIBYTE, 64LL * KIBIBYTE, 4LL * KIBIBYTE}, // SWRaid metadata 0.90 super block
        {-8LL * KIBIBYTE, 4LL * KIBIBYTE, 8LL * KIBIBYTE} // @-8K SWRaid metadata 1.0 super block
        // and @-4K Nilfs2 secondary super block
    };
    for (unsigned int i = 0; overallSuccess && i < sizeof(ranges) / sizeof(ranges[0]); i++) {
        //Rounding is performed in multiples of the sector size because writes are in whole sectors.

        Byte_Value roundingSize = Utils::ceilSize(ranges[i].rounding, lpDevice->sector_size);
        Byte_Value byteOffset;
        Byte_Value byteLen;
        if (ranges[i].offset >= 0LL) {
            byteOffset = Utils::floorSize(ranges[i].offset, roundingSize);
            byteLen = Utils::ceilSize(ranges[i].offset + ranges[i].length, lpDevice->sector_size) - byteOffset;
        } else { //Negative offsets
            Byte_Value notionalOffset = Utils::floorSize(partition.getByteLength() + ranges[i].offset, ranges[i].rounding);
            byteOffset = Utils::floorSize(notionalOffset, roundingSize);
            byteLen = Utils::ceilSize(notionalOffset + ranges[i].length, lpDevice->sector_size) - byteOffset;
        }
        //Limit range to partition size.
        if (byteOffset + byteLen <= 0LL) {
            //Byte range entirely before partition start.  Programmer error!
            continue;
        } else if (byteOffset < 0) {
            //Byte range spans partition start.  Trim to fit.
            byteLen += byteOffset;
            byteOffset = 0LL;
        }
        if (byteOffset >= partition.getByteLength()) {
            //Byte range entirely after partition end.  Ignore.
            continue;
        } else if (byteOffset + byteLen > partition.getByteLength()) {
            //Byte range spans partition end.  Trim to fit.
            byteLen = partition.getByteLength() - byteOffset;
        }

        Byte_Value written = 0LL;
        bool zeroSuccess = false;
        if (deviceIsOpen && buf) {
            // Start sector of the whole disk device or the partition
            Sector ptnStart = 0LL;
            if (lpPartition)
                ptnStart = lpPartition->geom.start;

            while (written < byteLen) {
                //Write in bufsize amounts.  Last write may be smaller but
                //  will still be a whole number of sectors.
                Byte_Value amount = std::min(bufsize, byteLen - written);
                zeroSuccess = ped_device_write(lpDevice, buf,
                                               ptnStart + (byteOffset + written) / lpDevice->sector_size,
                                               amount / lpDevice->sector_size);
                if (!zeroSuccess)
                    break;
                written += amount;
            }
        }
        overallSuccess &= zeroSuccess;
    }
    if (buf)
        free(buf);

    if (overallSuccess) {
        bool flushSuccess = false;
        if (deviceIsOpen) {
            flushSuccess = ped_device_sync(lpDevice);
            ped_device_close(lpDevice);
            //settleDevice(SETTLE_DEVICE_PROBE_MAX_WAIT_SECONDS);
        }
        overallSuccess &= flushSuccess;
    }
    destroyDeviceAndDisk(lpDevice, lpDisk);
    return overallSuccess;
}

bool PartedCore::setPartitionType(const Partition &partition)
{
    if (partition.m_type == TYPE_UNPARTITIONED)
        // Trying to set the type of a partition on a non-partitioned whole disk
        // device is a successful non-operation.
        return true;
    qDebug() << __FUNCTION__ << QString("set partition type on %1").arg(partition.getPath());
    //Set partition type appropriately for the type of file system stored in the partition.
    //  Libparted treats every type as a file system, except LVM which it treats as a flag.

    bool returnValue = false;

    PedDevice *lpDevice = nullptr;
    PedDisk *lpDisk = nullptr;
    if (getDeviceAndDisk(partition.m_devicePath, lpDevice, lpDisk)) {
        PedPartition *lpPartition = ped_disk_get_partition_by_sector(lpDisk, partition.getSector());
        if (lpPartition) {
            QString fsType = Utils::fileSystemTypeToString(partition.m_fstype);

            // Lookup libparted file system type using GParted's name, as most
            // match.  Exclude cleared as the name won't be recognised by
            // libparted and get_filesystem_string() has also translated it.
            PedFileSystemType *lpFsType = nullptr;
            if (partition.m_fstype != FS_CLEARED)
                lpFsType = ped_file_system_type_get(fsType.toLatin1());

            // If not found, and FS is udf, then try ntfs.
            // Actually MBR 07 IFS (Microsoft Installable File System) or
            // GPT BDP (Windows Basic Data Partition).
            // Ref: https://serverfault.com/a/829172
            if (!lpFsType && partition.m_fstype == FS_UDF)
                lpFsType = ped_file_system_type_get("ntfs");

            // default is Linux (83)
            if (!lpFsType)
                lpFsType = ped_file_system_type_get("ext2");

            bool supportsLvmFlag = ped_partition_is_flag_available(lpPartition, PED_PARTITION_LVM);

            if (lpFsType && partition.m_fstype != FS_LVM2_PV) {
                // Also clear any libparted LVM flag so that it doesn't
                // override the file system type
                if ((!supportsLvmFlag || ped_partition_set_flag(lpPartition, PED_PARTITION_LVM, 0)) && ped_partition_set_system(lpPartition, lpFsType) && commit(lpDisk)) {
                    qDebug() << __FUNCTION__ << QString("new partition type: %1").arg(lpPartition->fs_type->name);
                    returnValue = true;
                }
            } else if (partition.m_fstype == FS_LVM2_PV) {
                if (supportsLvmFlag && ped_partition_set_flag(lpPartition, PED_PARTITION_LVM, 1) && commit(lpDisk)) {
                    returnValue = true;
                } else if (!supportsLvmFlag) {
                    // Skip setting the lvm flag because the partition
                    // table type doesn't support it.  Applies to dvh
                    // and pc98 disk labels.
                    returnValue = true;
                }
            }
        }

        destroyDeviceAndDisk(lpDevice, lpDisk);
    }
    return returnValue;
}

bool PartedCore::createFileSystem(const Partition &partition)
{
    if (partition.m_fstype == FS_LUKS && partition.m_busy) {
        qDebug() << __FUNCTION__ << QString("partition contains open LUKS encryption for a create file system only step");
        return false;
    }
//    qDebug() << __FUNCTION__ << partition.m_sectorsUsed << partition.m_sectorsUnused;
    qDebug() << __FUNCTION__ << QString("create new %1 file system").arg(partition.m_fstype);
    bool succes = false;
    FileSystem *pFilesystem = nullptr;
    switch (getFileSystem(partition.m_fstype).create) {
    case FS::NONE:
        break;
    case FS::GPARTED:
        break;
    case FS::LIBPARTED:
        break;
    case FS::EXTERNAL: {
        succes = (pFilesystem = getFileSystemObject(partition.m_fstype)) && pFilesystem->create(partition);
        if (succes && !partition.getFileSystemLabel().isEmpty()) {
            pFilesystem->writeLabel(partition);
        }
    }
    break;
    default:
        break;
    }
    return succes;
}

bool PartedCore::createFileSystem(const FSType &type, const bool &busy, const QString path, const Partition &partition)
{
    //创建文件系统
    Partition p = partition;
    p.m_fstype = type;
    p.m_busy = false;
    p.setPath(path);
    return createFileSystem(p);
}

bool PartedCore::formatPartition(const Partition &partition)
{
    bool success = false;
    if (partition.m_fstype == FS_LUKS && partition.m_busy) {
        qDebug() << __FUNCTION__ << QString("partition contains open LUKS encryption for a format files system only step");
        return false;
    }

    if (partition.m_fstype == FS_CLEARED)
        success = eraseFilesystemSignatures(partition)
                  && setPartitionType(partition);
    else
        success = eraseFilesystemSignatures(partition)
                  && setPartitionType(partition)
                  && createFileSystem(partition);

    return success;
}

bool PartedCore::resize(const Partition &partitionNew)
{
    //ToDo fs linux-swap
    if (partitionNew.m_fstype == FS_LINUX_SWAP) {
        // linux-swap is recreated, not resized
        //        return    resizeMovePartition(partition_old, partition_new, operationdetail, true)
        //                  && recreate_linux_swap_filesystem(partition_new, operationdetail);
    }
    Sector delta = partitionNew.getSectorLength() - m_curpartition.getSectorLength();
    if (delta < 0LL) { // shrink
        //        return    check_repair_filesystem(partition_new)
        //                  && shrink_filesystem(curpartition, partition_new)
        //                  && resizeMovePartition(curpartition, partition_new, false);

        return checkRepairFileSystem(partitionNew)
               && resizeFileSystemImplement(m_curpartition, partitionNew)
               && resizeMovePartition(m_curpartition, partitionNew, true);

    } else if (delta > 0LL) { // grow
        return checkRepairFileSystem(partitionNew)
               && resizeMovePartition(m_curpartition, partitionNew, true)
               && maxImizeFileSystem(partitionNew);
    }
    return true;
}

bool PartedCore::checkRepairFileSystem(const Partition &partition)
{
    if (partition.m_fstype == FS_LUKS && partition.m_busy) {
        qDebug() << "partition contains open LUKS encryption for a check file system only step";
        return false;
    }

    if (partition.m_busy)
        // Trying to check an online file system is a successful non-operation.
        return true;
    qDebug() << QString("PartedCore::checkRepairFileSystem:check file system on %1"
                        " for errors and (if possible) fix them")
             .arg(partition.getPath());

    bool succes = false;
    FileSystem *pFilesystem = nullptr;
    switch (getFileSystem(partition.m_fstype).check) {
    case FS::NONE:
        qDebug() << "PartedCore::checkRepairFileSystem ,checking is not available for this file system";
        break;
    case FS::GPARTED:
        break;
    case FS::LIBPARTED:
        break;
    case FS::EXTERNAL:
        succes = (pFilesystem = getFileSystemObject(partition.m_fstype)) && pFilesystem->checkRepair(partition);
        break;
    default:
        break;
    }

    return succes;
}

bool PartedCore::resizeMovePartition(const Partition &partitionOld, const Partition &partitionNew, bool rollbackOnFail)
{
    Action action = NONE;
    if (partitionNew.getSectorLength() > partitionOld.getSectorLength())
        action = GROW;
    else if (partitionNew.getSectorLength() < partitionOld.getSectorLength())
        action = SHRINK;

    if (partitionNew.m_sectorStart > partitionOld.m_sectorStart && partitionNew.m_sectorEnd > partitionOld.m_sectorEnd)
        action = action == GROW ? MOVE_RIGHT_GROW : action == SHRINK ? MOVE_RIGHT_SHRINK : MOVE_RIGHT;
    else if (partitionNew.m_sectorStart < partitionOld.m_sectorStart && partitionNew.m_sectorEnd < partitionOld.m_sectorEnd)
        action = action == GROW ? MOVE_LEFT_GROW : action == SHRINK ? MOVE_LEFT_SHRINK : MOVE_LEFT;

    Sector newStart = -1;
    Sector newEnd = -1;
    bool success = resizeMovePartitionImplement(partitionOld, partitionNew, newStart, newEnd);
    if (!success && rollbackOnFail) {
        Partition *partitionIntersection = partitionNew.clone();
        partitionIntersection->m_sectorStart = std::max(partitionOld.m_sectorStart,
                                                        partitionNew.m_sectorStart);
        partitionIntersection->m_sectorEnd = std::min(partitionOld.m_sectorEnd,
                                                      partitionNew.m_sectorEnd);

        Partition *partitionRestore = partitionOld.clone();
        // Ensure that old partition boundaries are not modified
        partitionRestore->m_alignment = ALIGN_STRICT;

        resizeMovePartitionImplement(*partitionIntersection, *partitionRestore, newStart, newEnd);
        delete partitionRestore;
        partitionRestore = nullptr;
        delete partitionIntersection;
        partitionIntersection = nullptr;
    }

    return success;
}

bool PartedCore::resizeMovePartitionImplement(const Partition &partitionOld, const Partition &partitionNew, Sector &newStart, Sector &newEnd)
{
    bool success = false;
    PedDevice *lpDevice = nullptr;
    PedDisk *lpDisk = nullptr;
    if (getDeviceAndDisk(partitionOld.m_devicePath, lpDevice, lpDisk)) {
        PedPartition *lpPartition = getLpPartition(lpDisk, partitionOld);
        if (lpPartition) {
            PedConstraint *constraint = nullptr;
            if (partitionNew.m_alignment == ALIGN_STRICT || partitionNew.m_alignment == ALIGN_MEBIBYTE || partitionNew.m_strictStart) {
                PedGeometry *geom = ped_geometry_new(lpDevice,
                                                     partitionNew.m_sectorStart,
                                                     partitionNew.getSectorLength());
                if (geom) {
                    constraint = ped_constraint_exact(geom);
                    ped_geometry_destroy(geom);
                }
            } else {
                constraint = ped_constraint_any(lpDevice);
            }

            if (constraint) {
                if (ped_disk_set_partition_geom(lpDisk,
                                                lpPartition,
                                                constraint,
                                                partitionNew.m_sectorStart,
                                                partitionNew.m_sectorEnd)) {
                    newStart = lpPartition->geom.start;
                    newEnd = lpPartition->geom.end;

                    success = commit(lpDisk);
                }

                ped_constraint_destroy(constraint);
            }
        }
        destroyDeviceAndDisk(lpDevice, lpDisk);
    }

    return success;
}

bool PartedCore::maxImizeFileSystem(const Partition &partition)
{
    if (partition.m_fstype == FS_LUKS && partition.m_busy) {
        qDebug() << "PartedCore::maxImizeFileSystem: partition contains open LUKS encryption for a maximize file system only step";
        return false;
    }
    qDebug() << "PartedCore::maxImizeFileSystem: grow file system to fill the partition";

    // Checking if growing is available or allowed is only relevant for the check
    // repair operation to inform the user why the grow step is being skipped.  For a
    // resize/move operation these growing checks are merely retesting those performed
    // to allow the operation to be queued in the first place.  See
    // Win_GParted::set_valid_operations() and
    // Dialog_Partition_Resize_Move::Resize_Move_Normal().
    if (getFileSystem(partition.m_fstype).grow == FS::NONE) {
        qDebug() << "PartedCore::maxImizeFileSystem:growing is not available for this file system";
//        return true;
        return formatPartition(partition);
    }
    bool success = resizeFileSystemImplement(partition, partition);

    return success;
}

bool PartedCore::resizeFileSystemImplement(const Partition &partitionOld, const Partition &partitionNew)
{
    bool fillPartition = false;
    const FS &fsCap = getFileSystem(partitionNew.m_fstype);
    FS::Support action = FS::NONE;
    if (partitionNew.getSectorLength() >= partitionOld.getSectorLength()) {
        // grow (always maximises the file system to fill the partition)
        fillPartition = true;
        action = (partitionOld.m_busy) ? fsCap.online_grow : fsCap.grow;
    } else {
        // shrink
        fillPartition = false;
        action = (partitionOld.m_busy) ? fsCap.online_shrink : fsCap.shrink;
    }
    bool success = false;
    FileSystem *pFilesystem = nullptr;
    switch (action) {
    case FS::NONE:
        break;
    case FS::GPARTED:
        break;
    case FS::LIBPARTED:
        success = resizeMoveFileSystemUsingLibparted(partitionOld, partitionNew);
        break;
    case FS::EXTERNAL:
        success = (pFilesystem = getFileSystemObject(partitionNew.m_fstype)) && pFilesystem->resize(partitionNew, fillPartition);
        break;
    default:
        break;
    }

    return success;
}

bool PartedCore::resizeMoveFileSystemUsingLibparted(const Partition &partitionOld, const Partition &partitionNew)
{
    bool returnValue = false;
    PedDevice *lpDevice = nullptr;
    PedDisk *lpDisk = nullptr;
    if (getDeviceAndDisk(partitionOld.m_devicePath, lpDevice, lpDisk)) {
        PedFileSystem *fs = nullptr;
        PedGeometry *lpGeom = nullptr;

        lpGeom = ped_geometry_new(lpDevice,
                                  partitionOld.m_sectorStart,
                                  partitionOld.getSectorLength());
        if (lpGeom) {
            fs = ped_file_system_open(lpGeom);

            ped_geometry_destroy(lpGeom);
            lpGeom = nullptr;

            if (fs) {
                lpGeom = ped_geometry_new(lpDevice,
                                          partitionNew.m_sectorStart,
                                          partitionNew.getSectorLength());
                if (lpGeom) {
                    returnValue = ped_file_system_resize(fs, lpGeom, NULL);
                    if (returnValue)
                        commit(lpDisk);

                    ped_geometry_destroy(lpGeom);
                }
                ped_file_system_close(fs);
            }
        }
        destroyDeviceAndDisk(lpDevice, lpDisk);
    }

    return returnValue;
}

void PartedCore::onRefreshDeviceInfo(int type, bool arg1, QString arg2)
{
    qDebug() << " will call probeThread in thread !";
    if (m_workerThreadProbe == nullptr) {
        m_workerThreadProbe = new QThread();
        qDebug() << "onRefresh Create thread: " << QThread::currentThreadId() << " ++++++++" << m_workerThreadProbe << endl;
    }


    qDebug() << "  ----------------------! 0 !--------------------- :" << m_probeThread.thread();
    m_probeThread.moveToThread(m_workerThreadProbe);
    m_probeThread.setSignal(type, arg1, arg2);
    //connect(this, &PartedCore::probeAllInfo, &m_probeThread, &ProbeThread::probeDeviceInfo, Qt::UniqueConnection);
    m_workerThreadProbe->start();
    emit probeAllInfo();
    qDebug() << " called probeThread in thread !";
}

bool PartedCore::mountAndWriteFstab(const QString &mountpath)
{
    qDebug() << __FUNCTION__ << "Permanent mount start";
    QString type = Utils::fileSystemTypeToString(m_curpartition.m_fstype);
    bool success = mountDevice(mountpath, m_curpartition.getPath(),  m_curpartition.m_fstype)  //位置不可交换 利用&&运算特性
                   && writeFstab(m_curpartition.m_uuid, mountpath, type, true);
    qDebug() << __FUNCTION__ << "Permanent mount end";
    return   sendRefSigAndReturn(success);
}

bool PartedCore::unmount()
{
    //永久卸载
    qDebug() << __FUNCTION__ << "Unmount start";
    if (!umontDevice(m_curpartition.getMountPoints(), m_curpartition.getPath())) { //卸载挂载点  内部有信号发送 不要重复发送信号
        return false;
    }
    //修改/etc/fstab
    QString type = Utils::fileSystemTypeToString(m_curpartition.m_fstype);
    writeFstab(m_curpartition.m_uuid,  "", type, false);//非挂载 不需要挂载点
    qDebug() << __FUNCTION__ << "Unmount end";
    return sendRefSigAndReturn(true, DISK_SIGNAL_TYPE_UMNT, true, "1");
}

bool PartedCore::deCrypt(const LUKS_INFO &luks)
{
    LUKS_INFO luks2 =  luks;
    do {
        if (!m_LUKSInfo.luksExists(luks.m_devicePath)) {
            luks2.m_cryptErr = CRYPTError::CRYPT_ERR_DEVICE_NO_EXISTS;
            break;
        }
        luks2.isDecrypt = m_LUKSInfo.getLUKS(luks.m_devicePath).isDecrypt;

        if (!LUKSOperator::decrypt(m_LUKSInfo, luks2)) {
            luks2.m_cryptErr = CRYPTError::CRYPT_ERR_DECRYPT_FAILED;
            break;
        }
        probeDeviceInfo();
        if (!m_LUKSInfo.luksExists(luks.m_devicePath)) {
            luks2.m_cryptErr = CRYPTError::CRYPT_ERR_DEVICE_NO_EXISTS;
            break;
        }
        luks2 = m_LUKSInfo.getLUKS(luks.m_devicePath);
    } while (0);

    //发送解密是否成功信号
    luks2.m_decryptStr = luks.m_decryptStr;
    emit deCryptMessage(luks2);
    return luks2.m_cryptErr == CRYPTError::CRYPT_ERR_NORMAL;
}

bool PartedCore::cryptMount(const LUKS_INFO &luks)
{
    LUKS_INFO luks2 = luks;
    if (!m_LUKSInfo.luksExists(luks.m_devicePath)             //luks
            || !luks2.isDecrypt                               //判断是否解密
            || luks2.m_mapper.m_luksFs == FSType::FS_UNSUPPORTED    //判断文件系统是否可以支持
            || luks2.m_mapper.m_luksFs == FSType::FS_UNALLOCATED
            || luks2.m_mapper.m_luksFs == FSType::FS_UNKNOWN
            || luks2.m_mapper.m_luksFs == FSType::FS_UNFORMATTED) {
        return   sendRefSigAndReturn(false);
    }

    //mount
    foreach (const QString &mount, luks2.m_mapper.m_mountPoints) {
        if (!mountDevice(mount, luks2.m_mapper.m_dmPath, luks2.m_mapper.m_luksFs)) {
            return   sendRefSigAndReturn(false) ;
        }
    }

    //write confile
    if ((luks2.m_luksVersion == 1 && luks2.m_keySlots < LUKS1_MaxKey)  //判断密钥槽是否仍有空间
            || (luks2.m_luksVersion == 2 && luks2.m_keySlots < LUKS2_MaxKey)) {

        if (!LUKSOperator::addKeyAndCrypttab(m_LUKSInfo, luks2)) {      //判断写入crypttab文件是否成功
            return   sendRefSigAndReturn(false) ;
        }

        foreach (const QString &mount, luks2.m_mapper.m_mountPoints) {  //判断写入fstab文件是否成功
            if (!writeFstab(luks2.m_mapper.m_uuid, mount, Utils::fileSystemTypeToString(luks2.m_mapper.m_luksFs))) {
                return   sendRefSigAndReturn(false) ;
            }
        }
    }

    return   sendRefSigAndReturn(true);
}

bool PartedCore::cryptUmount(const LUKS_INFO &luks)
{
    LUKS_INFO luks2 = luks;
    if (!m_LUKSInfo.luksExists(luks2.m_devicePath) || !luks2.m_mapper.m_busy) {
        luks2.m_cryptErr = CRYPTError::CRYPT_ERR_DEVICE_NO_EXISTS;
        qDebug() << __FUNCTION__ << "Unmount CRYPT device error:device not exists or not mount";
        return sendRefSigAndReturn(true, DISK_SIGNAL_TYPE_UMNT, true, "0");
    }

    //卸载
    if (!umontDevice(luks2.m_mapper.m_mountPoints, luks2.m_mapper.m_dmName)) { //卸载挂载点  内部有信号发送 不要重复发送信号
        qDebug() << __FUNCTION__ << "Unmount CRYPT device error:device umount error";
        return false;
    }

    //判断写入fstab文件是否成功
    foreach (const QString &mount, luks2.m_mapper.m_mountPoints) {
        if (!writeFstab(luks2.m_mapper.m_uuid, mount, Utils::fileSystemTypeToString(luks2.m_mapper.m_luksFs), false)) {
            qDebug() << __FUNCTION__ << "Unmount CRYPT device error: writeFstab error";
        }
    }

    LUKSOperator::removeMapperAndKey(m_LUKSInfo, luks2);
    qDebug() << __FUNCTION__ << "Unmount CRYPT device end";
    return sendRefSigAndReturn(true, DISK_SIGNAL_TYPE_UMNT, true, "1");
}

bool PartedCore::create(const PartitionVec &infovec)
{
    qDebug() << __FUNCTION__ << "Create start";
    bool success = true;
    for (PartitionInfo info : infovec) {
        Partition newPartition;
        newPartition.reset(info);
        if (!create(newPartition)) {
            qDebug() << __FUNCTION__ << "Create Partitione error";
            success = false;
            break;
        }
    }
    if (!m_isClear) {
        emit refreshDeviceInfo();
    }

    qDebug() << __FUNCTION__ << "Create end";
    return success;
}

bool PartedCore::create(Partition &newPartition)
{
    bool success = false;
    if (newPartition.m_type == TYPE_EXTENDED) {
        success = createPartition(newPartition);
    } else {
        FS_Limits fsLimits = getFileSystemLimits(newPartition.m_fstype, newPartition);
        success = createPartition(newPartition, fsLimits.min_size / newPartition.m_sectorSize);
    }
    if (!success)
        return false;

    if (!newPartition.m_name.isEmpty()) {
        if (!namePartition(newPartition))
            return false;
    }

    //判断是否加密 加密
    if (newPartition.m_luksFlag == LUKSFlag::NOT_CRYPT_LUKS) {
        if (newPartition.m_type == TYPE_EXTENDED || newPartition.m_fstype == FS_UNFORMATTED)
            return true;
        else if (newPartition.m_fstype == FS_CLEARED)
            return eraseFilesystemSignatures(newPartition);
        else
            return eraseFilesystemSignatures(newPartition)
                   && setPartitionType(newPartition)
                   && createFileSystem(newPartition);
    } else {
        success = eraseFilesystemSignatures(newPartition)
                  && setPartitionType(newPartition);

        if (!success) {
            return false;
        }

        LUKS_INFO info = getNewLUKSInfo(newPartition);
        return LUKSOperator::encrypt(m_LUKSInfo, info)
               && LUKSOperator::decrypt(m_LUKSInfo, info)
               && createFileSystem(info.m_mapper.m_luksFs, false, info.m_mapper.m_dmPath)
               && LUKSOperator::closeMapper(m_LUKSInfo, info);
    }
}

bool PartedCore::createPartition(Partition &newPartition, Sector minSize)
{
    qDebug() << __FUNCTION__ << "create empty partition from:" << newPartition.m_devicePath;
    newPartition.m_partitionNumber = 0;
    PedDevice *lpDevice = nullptr;
    PedDisk *lpDisk = nullptr;

    //对应 Bug 95232, 如果检测到虚拟磁盘扩容的话，重新写一下分区表，就可以修正分区表数据.
    reWritePartition(newPartition.m_devicePath);
//    QString cmd = QString("fdisk -l %1 2>&1").arg(newPartition.m_devicePath);
//    FILE *fd = nullptr;
//    fd = popen(cmd.toStdString().data(), "r");
//    char pb[1024];
//    memset(pb, 0, 1024);

//    if (fd) {
//        while (fgets(pb, 1024, fd) != nullptr) {

//            if (strstr(pb, "will be corrected by write")) {
//                QString cmd_fix = QString("echo w | fdisk %1").arg(newPartition.m_devicePath);

//                FILE *fixfd = popen(cmd_fix.toStdString().data(), "r");
//                if (fixfd) {
//                    pclose(fixfd);
//                }
//                qDebug() << __FUNCTION__ << "createPartition Partition Table Rewrite Done";
//            }
//        }

//        pclose(fd);
//    }


    if (getDeviceAndDisk(newPartition.m_devicePath, lpDevice, lpDisk)) {
        PedPartitionType type;
        PedConstraint *constraint = nullptr;
        PedFileSystemType *fsType = nullptr;
        //create new partition
        switch (newPartition.m_type) {
        case TYPE_PRIMARY:
            type = PED_PARTITION_NORMAL;
            break;
        case TYPE_LOGICAL:
            type = PED_PARTITION_LOGICAL;
            break;
        case TYPE_EXTENDED:
            type = PED_PARTITION_EXTENDED;
            break;

        default:
            type = PED_PARTITION_FREESPACE;
        }
        if (newPartition.m_type != TYPE_EXTENDED)
            fsType = ped_file_system_type_get("ext2");

        PedPartition *lpPartition = ped_partition_new(lpDisk, type, fsType,
                                                      newPartition.m_sectorStart, newPartition.m_sectorEnd);

        if (lpPartition) {
            if (newPartition.m_alignment == ALIGN_STRICT
                    || newPartition.m_alignment == ALIGN_MEBIBYTE) {
                PedGeometry *geom = ped_geometry_new(lpDevice, newPartition.m_sectorStart, newPartition.getSectorLength());
                if (geom) {
                    constraint = ped_constraint_exact(geom);
                    ped_geometry_destroy(geom);
                }
            } else
                constraint = ped_constraint_any(lpDevice);

            if (constraint) {
                if (minSize > 0 && newPartition.m_fstype != FS_XFS) // Permit copying to smaller xfs partition
                    constraint->min_size = minSize;

                if (ped_disk_add_partition(lpDisk, lpPartition, constraint) && commit(lpDisk)) {
                    newPartition.setPath(getPartitionPath(lpPartition));

                    newPartition.m_partitionNumber = lpPartition->num;
                    newPartition.m_sectorStart = lpPartition->geom.start;
                    newPartition.m_sectorEnd = lpPartition->geom.end;
                }
                ped_constraint_destroy(constraint);
            }
        }
        destroyDeviceAndDisk(lpDevice, lpDisk);
    }
    return newPartition.m_partitionNumber > 0;
}

void PartedCore::reWritePartition(const QString &devicePath)
{

    struct stat fileStat;
    stat(devicePath.toStdString().c_str(), &fileStat);
    if (!S_ISBLK(fileStat.st_mode)) {
        qDebug() << __FUNCTION__ << QString("%1 is not blk file").arg(devicePath);
        return;
    }

    bool needRewrite = gptIsExpanded(devicePath);
    if (needRewrite) {
        QString outPutFix, errorFix;
        QString cmdFix = QString("echo w | fdisk %1").arg(devicePath);
        Utils::executWithPipeCmd(cmdFix, outPutFix, errorFix);
        qDebug() << __FUNCTION__ << "createPartition Partition Table Rewrite Done";
        return;
    }
}
/**********************************lvm**********************************/
QList<PVData> PartedCore::getCreatePVList(const QList<PVData> &devList, const long long &totalSize)
{
    QList<PVData>diskList;
    QList<PVData>partList;
    QList<PVData>unallocList;
    QList<PVData>loopList;
    QList<PVData>metaList;

    //默认分区最小大小
    long long minPartSize =  6 * MEBIBYTE;

    long long unallocSize = totalSize; //去掉分区大小后的未分配空间
    foreach (PVData pv, devList) {
        long long pvSize = getPVSize(pv);
        if (pvSize <= 0)
            continue;
        if (pv.m_type == DevType::DEV_DISK) {
            diskList.push_back(pv);
        } else if (pv.m_type == DevType::DEV_PARTITION) {
            unallocSize -= pvSize;
            partList.push_back(pv);
        } else if (pv.m_type == DevType::DEV_UNALLOCATED_PARTITION) {
            unallocList.push_back(pv);
        } else if (pv.m_type == DevType::DEV_LOOP) {
            loopList.push_back(pv);
        } else if (pv.m_type == DevType::DEV_META_DEVICES) {
            metaList.push_back(pv);
        }
    }

    //排序
    auto sortFunc = [ = ](const PVData & pv1, const PVData & pv2)->bool{
        long long pv1Size = pv1.m_sectorSize * (pv1.m_endSector - pv1.m_startSector + 1);
        long long pv2Size = pv2.m_sectorSize * (pv2.m_endSector - pv2.m_startSector + 1);

        return pv1Size > pv2Size;
    };

    std::sort(unallocList.begin(), unallocList.end(), sortFunc);
    std::sort(diskList.begin(), diskList.end(), sortFunc);

    //优先使用 分区
    QList<PVData>list = partList;
    //使用未分配空间 unallocList
    foreach (PVData pv, unallocList) {
        if (unallocSize <= 0) {
            break;
        }
        long long pvSize = getPVSize(pv);
        if (pvSize <= 0) {
            continue;
        }

        if (unallocSize < pvSize) {
            if (unallocSize <= minPartSize) {
                unallocSize = minPartSize;
            }
        }

        if (!getPVStartEndSector(pv, unallocSize)) {
            return  QList<PVData>();
        }

        //创建分区
        if (!createPVPart(pv)) {
            return QList<PVData>();
        }

        unallocSize -= pvSize;
        list.push_back(pv);
    }
    //磁盘
    foreach (PVData pv, diskList) {
        if (unallocSize <= 0) {
            break;
        }
        long long pvSize = getPVSize(pv);
        long long pvSize2 = getPVSize(pv, true);
        if (pvSize <= 0)
            continue;
        if (unallocSize < pvSize && unallocSize < pvSize2) { //获取创建分区情况下的size
            if (unallocSize <= minPartSize) {
                unallocSize = minPartSize;
            }
            if (!getPVStartEndSector(pv, unallocSize)) {
                return  QList<PVData>();
            }
            //创建分区
            if (!createPVPart(pv)) {
                return QList<PVData>();
            }
        }
        list.push_back(pv);
        unallocSize -= pvSize;
    }
    if (unallocSize > 0)
        return QList<PVData>();

    //磁盘
    foreach (PVData pv, list) {
        if (pv.m_type == DEV_PARTITION) {
            //todo 修改分区类型
        }

    }

//    auto printPV = [ = ](QString name, QList<PVData>pvList) {
//        qDebug() << "__FUNCTION__" << "-------------------------";
//        qDebug() << "__FUNCTION__" << name;
//        foreach (PVData pv, pvList) {
//            qDebug() << "__FUNCTION__"  << pv.m_diskPath;
//            qDebug() << "__FUNCTION__"  << pv.m_startSector;
//            qDebug() << "__FUNCTION__"  << pv.m_devicePath;
//            qDebug() << "__FUNCTION__"  << pv.m_endSector;
//            qDebug() << "__FUNCTION__"  << pv.m_sectorSize;
//        }
//        qDebug() << "__FUNCTION__" << "-------------------------";
//    };


//    printPV("devList", devList);
//    printPV("partList", partList);
//    printPV("unallocList", unallocList);
//    printPV("diskList", diskList);
//    printPV("loopList", loopList);
//    printPV("metaList", metaList);
//    printPV("list", list);

    return list;
}

QList<PVData> PartedCore::getResizePVList(const QString &vgName, const QList<PVData> &devList, const long long &totalSize)
{
    if (!m_lvmInfo.vgExists(vgName)) { //vg不存在
        return QList<PVData> ();
    }

    VGInfo vg = m_lvmInfo.getVG(vgName);
    if (vg.m_peUsed * vg.m_PESize > totalSize) { //调整数值有误  已使用 > 总大小
        return QList<PVData> ();
    }

    //默认分区最小大小
    long long minPartSize = vg.m_PESize + 2 * MEBIBYTE;

    //添加
    QList<PVData>addDiskList;
    QList<PVData>addPartList;
    QList<PVData>addUnallocList;
    QList<PVData>addLoopList;
    QList<PVData>addMetaList;

    //已有 oldList[0]=> disk oldList[1]=> part
    QList<PVData>oldList[2];

    foreach (PVData pv, devList) {
        bool oldFlag = m_lvmInfo.pvOfVg(vgName, pv);;
        if (pv.m_type == DevType::DEV_DISK) {  //磁盘 如果已经加入pv
            oldFlag ? oldList[0].push_back(pv) : addDiskList.push_back(pv);
        } else if (pv.m_type == DevType::DEV_PARTITION) {
            oldFlag ? oldList[1].push_back(pv) : addPartList.push_back(pv);
        } else if (pv.m_type == DevType::DEV_UNALLOCATED_PARTITION) {
            addUnallocList.push_back(pv);
        } else if (pv.m_type == DevType::DEV_LOOP) {
            addLoopList.push_back(pv);
        } else if (pv.m_type == DevType::DEV_META_DEVICES) {
            addMetaList.push_back(pv);
        }
    }

    //排序
    auto sortFunc = [ = ](const PVData & pv1, const PVData & pv2)->bool{
        long long pv1Size = pv1.m_sectorSize * (pv1.m_endSector - pv1.m_startSector + 1);
        long long pv2Size = pv2.m_sectorSize * (pv2.m_endSector - pv2.m_startSector + 1);

        return pv1Size > pv2Size;
    };

    std::sort(addDiskList.begin(), addDiskList.end(), sortFunc);
    std::sort(addPartList.begin(), addPartList.end(), sortFunc);
    std::sort(addUnallocList.begin(), addUnallocList.end(), sortFunc);
    std::sort(addLoopList.begin(), addLoopList.end(), sortFunc);
    std::sort(addMetaList.begin(), addMetaList.end(), sortFunc);
    std::sort(oldList[0].begin(), oldList[0].end(), sortFunc);
    std::sort(oldList[1].begin(), oldList[1].end(), sortFunc);

    QList<PVData>list;
    long long unallocSize = totalSize;
    //优先使用保留的disk 和part
    //根据前端逻辑 所有的分区和pv都将要使用 所以此处这样写 如果前端逻辑修改后 此处需要修改
    for (int i = 0; i < 2; ++i) {
        foreach (const PVData &pv, oldList[i]) {
            unallocSize -= getPVSize(pv);
            list.push_back(pv);
        }
    }
    //使用分区
    foreach (PVData pv, addPartList) {
        unallocSize -= getPVSize(pv);
        list.push_back(pv);
    }

    if (unallocSize <= 0)
        return list;


    //使用未分配空间 unallocList
    foreach (PVData pv, addUnallocList) {
        if (unallocSize <= 0) {
            break;
        }
        long long pvSize = getPVSize(pv);
        if (pvSize <= 0) {
            continue;
        }

        if (unallocSize < pvSize) {
            if (unallocSize <= minPartSize) {
                unallocSize = minPartSize;
            }
        }

        if (!getPVStartEndSector(pv, unallocSize)) {
            return  QList<PVData>();
        }

        //创建分区
        if (!createPVPart(pv)) {
            return QList<PVData>();
        }

        unallocSize -= pvSize;
        list.push_back(pv);
    }


    if (unallocSize <= 0)
        return list;

    //磁盘
    foreach (PVData pv, addDiskList) {
        if (unallocSize <= 0) {
            break;
        }
        long long pvSize = getPVSize(pv);
        long long pvSize2 = getPVSize(pv, true);
        if (pvSize <= 0)
            continue;


        if (unallocSize < pvSize && unallocSize < pvSize2) { //获取创建分区情况下的size
            if (unallocSize <= minPartSize) {
                unallocSize = minPartSize;
            }
            if (!getPVStartEndSector(pv, unallocSize)) {
                return  QList<PVData>();
            }
            //创建分区
            if (!createPVPart(pv)) {
                return QList<PVData>();
            }
        }

//        if (unallocSize < pvSize) {
//            if (!getPVStartEndSector(pv, unallocSize)) {
//                return  QList<PVData>();
//            }
//            //创建分区
//            if (!createPVPart(pv)) {
//                return QList<PVData>();
//            }
//        }
        unallocSize -= pvSize;

        list.push_back(pv); //整盘加入 从头到尾都可用
    }
    if (unallocSize > 0)
        return QList<PVData>();

    //磁盘
    foreach (PVData pv, list) {
        if (pv.m_type == DEV_PARTITION) {
            //todo 修改分区类型
        }
    }
    return list;
}

bool PartedCore::getPVDevice(const QString &devPath, Device &device)
{
    auto it = m_deviceMap.find(devPath);
    if (it != m_deviceMap.end()) {
        device = *it;
    }
    return it != m_deviceMap.end();
}

long long PartedCore::getPVSize(const PVData &pv, bool flag)
{
    Q_UNUSED(flag)
    Device dev;
    if (!getPVDevice(pv.m_diskPath, dev)) {
        return -1;
    }
    long long startSec = pv.m_startSector;
    long long endSec = pv.m_endSector;

    //以下注释代码为获取设备真实可用大小 但是与目前业务有冲突 所以改为获取磁盘真实的大小（分区表以及gpt格式开头结尾不再去除）

//    if (startSec == 0 && pv.m_type == LVM_DEV_UNALLOCATED_PARTITION) {
//        startSec = UEFI_SECTOR;
//    }

//    if (pv.m_type == LVM_DEV_UNALLOCATED_PARTITION && pv.m_endSector == (dev.m_length - 1) && dev.m_diskType.contains("gpt")) {
//        endSec -= GPTBACKUP;
//    }


//    if (pv.m_type == LVM_DEV_DISK && flag) {
//        startSec -= UEFI_SECTOR;
//        endSec -= GPTBACKUP;
//    }

    return dev.m_sectorSize * (endSec - startSec + 1);
}

bool PartedCore::getPVStartEndSector(PVData &pv, const long long &unallocaSize)
{
    Device dev;
    if (!getPVDevice(pv.m_diskPath, dev)) {
        return false;
    }

    long long startSec = pv.m_startSector;
    long long endSec = pv.m_endSector;
    long long allSec = unallocaSize / dev.m_sectorSize + 1; //总共需要的扇区
    long long pvSize = getPVSize(pv);

    if (pv.m_type == DEV_DISK || (pv.m_type == DEV_UNALLOCATED_PARTITION && pv.m_startSector == 0)) { //如果是磁盘或者有分区表但是没有分区 要从2048扇区开始
        startSec = UEFI_SECTOR;
    }

    if ((pv.m_type == DEV_UNALLOCATED_PARTITION && pv.m_endSector == (dev.m_length - 1) && dev.m_diskType.contains("gpt")) || pv.m_type == DEV_DISK) {
        endSec -= GPTBACKUP; //gpt格式最后33个扇区不可使用
    }

    /*
     * 如果是磁盘加入又要创建分区表的情况 那么只可能进入if分支
     * 因为首先算出磁盘的大小比unalloca大才会调用该函数 否则磁盘就会整盘加入
    */
    if (unallocaSize < pvSize) { //需要的大小比当前未分配分区小
        if ((endSec - startSec + 1) < allSec) {
            return false;
        }
        endSec = startSec + allSec - 1;
    }

    pv.m_startSector = startSec;
    pv.m_endSector = endSec;

    return true;
}

bool PartedCore::createPVPartition(PVData &pv)
{
    PedDevice *lpDevice = ped_device_get(pv.m_diskPath.toStdString().c_str());  //根据路径获取磁盘设备
    if (nullptr == lpDevice)
        return false;
    PedDisk *Disk = ped_disk_new(lpDevice); //根据磁盘获取块设备
    if (nullptr == Disk)
        return false;
    PedPartitionType type = PED_PARTITION_NORMAL;
    PedConstraint *constraint = nullptr;
    PedFileSystemType *fsType = ped_file_system_type_get("ext2");
    if (nullptr == fsType)
        return false;
    PedPartition *lpPartition = ped_partition_new(Disk, type, fsType, pv.m_startSector, pv.m_endSector);

    if (nullptr == lpPartition)
        return false;

    PedGeometry *geom = ped_geometry_new(lpDevice, pv.m_startSector, pv.m_endSector - pv.m_startSector + 1);
    if (geom) {
        constraint = ped_constraint_exact(geom);
        ped_geometry_destroy(geom);
    }
    if (constraint) {
        ped_disk_add_partition(Disk, lpPartition, constraint);
        ped_device_open(Disk->dev);
        ped_disk_commit_to_dev(Disk);
        ped_disk_commit_to_os(Disk);
        ped_device_close(Disk->dev);
        ped_constraint_destroy(constraint);
    }

    pv.m_devicePath = QString("%1%2").arg(pv.m_diskPath).arg(lpPartition->num);
    if (Disk)
        ped_disk_destroy(Disk);

    if (lpDevice)
        ped_device_destroy(lpDevice);
    return true;
}

bool PartedCore::createPVPart(PVData &pv)
{
    Device dev;
    if (!getPVDevice(pv.m_diskPath, dev)) {
        return false;
    }

    if (pv.m_type == DEV_UNALLOCATED_PARTITION) {
        if (!createPVPartition(pv)) { //创建分区
            return false;
        };
    } else  if (pv.m_type == DEV_DISK) {
        if (!createPartitionTable(dev.m_path, QString("%1").arg(dev.m_length), QString("%1").arg(dev.m_sectorSize), "gpt")) { //创建分区表
            return false;
        }
        if (!createPVPartition(pv)) { //创建分区
            return false;
        };
    }
    pv.m_type = DEV_PARTITION;
    return true;
}

LUKS_INFO PartedCore::getNewLUKSInfo(const Partition &part)
{
    QString dmName = part.m_dmName;
    if (dmName.isEmpty()) {
        QString crypt;
        if (part.m_cryptCipher == CRYPT_CIPHER::AES_XTS_PLAIN64) {
            crypt = "aesE";
        } else if (part.m_cryptCipher == CRYPT_CIPHER::SM4_XTS_PLAIN64) {
            crypt = "sm4E";
        }
        dmName = part.getPath().replace("/dev/", "") + "_" + crypt;
    }

    return getNewLUKSInfo(part.m_fstype,
                          part.m_tokenList,
                          part.m_cryptCipher,
                          part.m_decryptStr,
                          dmName,
                          part.getPath(),
                          part.getFileSystemLabel());
}

LUKS_INFO PartedCore::getNewLUKSInfo(const LVAction &lvAct)
{
    return getNewLUKSInfo(lvAct.m_lvFs,
                          lvAct.m_tokenList,
                          lvAct.m_crypt,
                          lvAct.m_decryptStr,
                          lvAct.m_dmName,
                          QString("/dev/mapper/%1-%2").arg(lvAct.m_vgName).arg(lvAct.m_lvName),
                          "");
}

LUKS_INFO PartedCore::getNewLUKSInfo(const FSType &type, const QStringList &token, const CRYPT_CIPHER &cipher, const QString &decryptStr, const QString &dmName, const QString devPath, const QString &label)
{
    LUKS_INFO info;
    info.m_mapper.m_luksFs = type;
    info.m_tokenList = token;
    info.m_crypt = cipher;
    info.m_decryptStr = decryptStr;
    info.m_mapper.m_dmName = dmName;
    info.m_mapper.m_dmPath = "/dev/mapper/" + dmName;
    info.m_devicePath = devPath;
    info.m_fileSystemLabel = label;
    return info;
}

bool PartedCore::checkPVDevice(const QString vgName, const PVData &pv, bool isCreate)
{
    if (pv.m_type == DevType::DEV_UNKNOW_DEVICES) { //设备类型不明
        return false;
    }

    auto devIt = m_deviceMap.find(pv.m_diskPath); //设备不存在
    if (devIt == m_deviceMap.end()) {
        return false;
    }

    const QVector<Partition *> &part = devIt.value().m_partitions;
    QVector<Partition *>::const_iterator partIt = std::find_if(part.begin(), part.end(), [ = ](Partition * it)->bool{
        if (it->m_type != TYPE_UNALLOCATED)
        {
            return it->getPath() == pv.m_devicePath;
        } else
        {
            return it->m_sectorEnd == pv.m_endSector && it->m_sectorStart == pv.m_startSector;
        }
    });

    if (m_lvmInfo.pvExists(pv)) { //pv已经存在
        PVInfo info = m_lvmInfo.getPV(pv);
        bool noJoinVG = info.noJoinVG();
        /*如果是创建vg 那么判断pv没有加入任何vg就可以
         * 如果不是创建vg而是resize 那么pv没有加入任何vg或者pv属于当前vg都是可以的
         *                       加入vg 且不属于当前vg的pv是错误的参数*/
        return isCreate ? noJoinVG : (noJoinVG || m_lvmInfo.pvOfVg(vgName, info));
    }

    switch (pv.m_type) { //判断设备是否符合
    case DevType::DEV_DISK: { //验证是否存在分区表
        if (devIt.value().m_diskType != "unrecognized") {
            return false;
        }
    }
    break;
    case DevType::DEV_PARTITION: { //验证分区是否存在 是否被挂载
        if (part.end() == partIt || !(*partIt)->getMountPoint().isEmpty()) { //此处两个判断位置不可交换 交换会崩溃
            return false;
        }
    }
    break;
    case DevType::DEV_UNALLOCATED_PARTITION: { //验证未分配分区是否存在
        if (part.end() == partIt) {
            return false;
        }
    }
    break;
    case DevType::DEV_LOOP: //暂时不做处理 以后支持时添加
        break;
    case DevType::DEV_META_DEVICES://暂时不做处理 以后支持时添加
        break;
    default:
        return false;
    }
    return true;
}

void PartedCore::deletePVListMessage(bool flag)
{
    m_lvmInfo.m_lvmErr = LVMOperator::m_lvmErr;
    sendRefSigAndReturn(flag, DISK_SIGNAL_TYPE_PVDELETE, flag, QString("%1:%2").arg(flag ? 1 : 0).arg(m_lvmInfo.m_lvmErr));
}

void PartedCore::resizeVGMessage(bool flag)
{
    m_lvmInfo.m_lvmErr = LVMOperator::m_lvmErr;
    sendRefSigAndReturn(flag, DISK_SIGNAL_TYPE_VGCREATE, flag, QString("%1:%2").arg(flag ? 1 : 0).arg(m_lvmInfo.m_lvmErr));
}

bool PartedCore::createVG(QString vgName, QList<PVData> devList, long long size)
{
    std::set<PVData>tmpDevList;
    foreach (PVData pv, devList) {
        if (!tmpDevList.insert(pv).second || !checkPVDevice(vgName, pv, true)) { //插入失败说明有重复
            return sendRefSigAndReturn(false, DISK_SIGNAL_TYPE_VGCREATE, true, QString("0:%1").arg(LVM_ERR_VG_ARGUMENT));
        }
    }

    QList<PVData>list =  getCreatePVList(devList, size);

    // find luks Device and Close
    foreach (const PVData &pv, list) {
        if (!m_LUKSInfo.luksExists(pv.m_devicePath)) {
            continue;
        }
        LUKS_INFO luks = m_LUKSInfo.getLUKS(pv.m_devicePath);
        LUKSOperator::removeMapperAndKey(m_LUKSInfo, luks);
    }

    if (!LVMOperator::createVG(m_lvmInfo, vgName, list, size)) {
        return sendRefSigAndReturn(false, DISK_SIGNAL_TYPE_VGCREATE, true, QString("0:%1").arg(m_lvmInfo.m_lvmErr));
    }
    return sendRefSigAndReturn(true, DISK_SIGNAL_TYPE_VGCREATE, true, QString("1:%1").arg(m_lvmInfo.m_lvmErr));
}

bool PartedCore::createLV(QString vgName, QList<LVAction> lvList)
{
    foreach (const LVAction &clv, lvList) {
        if (!supportedFileSystem(clv.m_lvFs)) { //判断文件系统是否支持
            return sendRefSigAndReturn(false);
        }
    }

    //创建lv 失败返回false
    if (!LVMOperator::createLV(m_lvmInfo, vgName, lvList)) {
        return sendRefSigAndReturn(false);
    }

    //获取新创建lv列表
    QVector<LVInfo>lvVec;
    QVector<LVAction>lvVecActDecrypt;
    QVector<LVInfo>lvVecDecrypt;
    VGInfo vg = m_lvmInfo.getVG(vgName);
    foreach (const LVAction &clv, lvList) {
        if (!vg.lvInfoExists(clv.m_lvName)) {
            continue;
        }

        LVInfo lv = vg.getLVinfo(clv.m_lvName);
        lv.m_lvFsType = clv.m_lvFs;
        if (clv.m_luksFlag == LUKSFlag::NOT_CRYPT_LUKS) {
            lvVec.push_back(lv);
        } else {
            lvVecDecrypt.push_back(lv);
            lvVecActDecrypt.push_back(clv);
        }
    }

    QString userName = lvList.begin()->m_user;
    if (userName.trimmed().isEmpty()) {
        return sendRefSigAndReturn(false);
    }

    foreach (const LVInfo &lvInfo, lvVec) {
        if (lvInfo.m_lvUuid.trimmed().isEmpty()) {
            return sendRefSigAndReturn(false);
        }
        //创建文件系统
        if (!createFileSystem(lvInfo.m_lvFsType, lvInfo.m_busy, QString("/dev/%1/%2").arg(lvInfo.m_vgName).arg(lvInfo.m_lvName))) {
            return sendRefSigAndReturn(false);
        }
        //自动挂载 获取挂载文件夹名字
        QString mountPath = QString("/media/%1/%2").arg(userName).arg(lvInfo.m_lvUuid);
        if (!tmpMountDevice(mountPath, lvInfo.m_lvPath, lvInfo.m_lvFsType, userName).first) {
            return sendRefSigAndReturn(false);
        }
    }


    //新建加密
    foreach (const LVAction &lv, lvVecActDecrypt) {
        LUKS_INFO info = getNewLUKSInfo(lv);
        if (!(LUKSOperator::encrypt(m_LUKSInfo, info)
                && LUKSOperator::decrypt(m_LUKSInfo, info)
                && createFileSystem(info.m_mapper.m_luksFs, info.m_mapper.m_busy, info.m_mapper.m_dmPath))) {
            return sendRefSigAndReturn(false);
        }
    }

    probeDeviceInfo();
    foreach (const LVInfo &lv, lvVecDecrypt) {
        LUKS_INFO info = m_LUKSInfo.getLUKS(lv.m_lvPath);
        //自动挂载 获取挂载文件夹名字
        QString mountPath = QString("/media/%1/%2").arg(userName).arg(info.m_mapper.m_uuid);
        if (!tmpMountDevice(mountPath, info.m_mapper.m_dmPath, info.m_mapper.m_luksFs, userName).first) {
            return sendRefSigAndReturn(false);
        }
    }
    return sendRefSigAndReturn(true);
}

bool PartedCore::deleteVG(QStringList vglist)
{
    bool flag = LVMOperator::deleteVG(m_lvmInfo, vglist);
    QString str = flag ? "1:0" : QString("0:%1").arg(m_lvmInfo.m_lvmErr);
    return sendRefSigAndReturn(flag, DISK_SIGNAL_TYPE_VGDELETE, flag, str);
}

bool PartedCore::deleteLV(QStringList lvlist)
{
    // find luks Device and Close
    foreach (const QString &str, lvlist) {
        if (!m_LUKSInfo.luksExists(str)) {
            continue;
        }
        LUKS_INFO luks = m_LUKSInfo.getLUKS(str);
        LUKSOperator::removeMapperAndKey(m_LUKSInfo, luks);
    }

    bool flag = LVMOperator::lvRemove(m_lvmInfo, lvlist);
    QString str = flag ? "1:0" : QString("0:%1").arg(m_lvmInfo.m_lvmErr);
    return sendRefSigAndReturn(flag, DISK_SIGNAL_TYPE_LVDELETE, flag, str);
}

bool PartedCore::resizeVG(QString vgName, QList<PVData> devList, long long size)
{
    std::set<PVData>tmpDevList;
    foreach (PVData pv, devList) {
        if (!tmpDevList.insert(pv).second || !checkPVDevice(vgName, pv, false)) { //插入失败说明有重复
            return sendRefSigAndReturn(false, DISK_SIGNAL_TYPE_VGCREATE, true, QString("0:%1").arg(LVM_ERR_VG_ARGUMENT));
        }
    }

    if (m_workerLVMThread == nullptr) {
        m_workerLVMThread = new QThread();
        m_workerLVMThread->start();
        m_lvmThread.moveToThread(m_workerLVMThread);
    }

    QList<PVData>list =  getResizePVList(vgName, devList, size);


    // find luks Device and Close
    foreach (const PVData &pv, list) {
        if (!m_LUKSInfo.luksExists(pv.m_devicePath)) {
            continue;
        }
        LUKS_INFO luks = m_LUKSInfo.getLUKS(pv.m_devicePath);
        LUKSOperator::removeMapperAndKey(m_LUKSInfo, luks);
    }

    if (list.size() <= 0) {
        return sendRefSigAndReturn(false, DISK_SIGNAL_TYPE_VGCREATE, true, QString("0:%1").arg(LVMError::LVM_ERR_VG_ARGUMENT));
    }

    //线程启动信号
    emit resizeVGStart(m_lvmInfo, vgName, list, size);
    return true;
}

bool PartedCore::resizeLV(LVAction &lvAct)
{
    //lv是否存在
    if (! m_lvmInfo.lvInfoExists(lvAct.m_vgName, lvAct.m_lvName)) {
        return sendRefSigAndReturn(false);
    }

    //执行动作是否为扩大或缩小
    if (!(lvAct.m_lvAct == LVM_ACT_LV_EXTEND || lvAct.m_lvAct == LVM_ACT_LV_REDUCE)) {
        return sendRefSigAndReturn(false);
    }

    //调整大小是否小于文件系统最小值 或者等于当前大小
    LVInfo info = m_lvmInfo.getLVInfo(lvAct.m_vgName, lvAct.m_lvName);
    if (lvAct.m_lvByteSize < info.m_fsLimits.min_size || lvAct.m_lvByteSize == (info.m_lvLECount * info.m_LESize)) {
        return sendRefSigAndReturn(false);
    }

    if (lvAct.m_lvAct == LVM_ACT_LV_REDUCE) { //缩小
        //自动卸载
        if (!umontDevice(info.m_mountPoints, info.toMapperPath())) {
            return sendRefSigAndReturn(false);
        }
    }

    //调整lv大小
    if (!LVMOperator::resizeLV(m_lvmInfo, lvAct, info)) {
        return sendRefSigAndReturn(false);
    }

    if (lvAct.m_lvAct == LVM_ACT_LV_REDUCE && info.m_mountPoints.size() >= 1) {
        //调整lv大小
        if (!mountDevice(info.m_mountPoints[0], info.m_lvPath, info.m_lvFsType)) {
            return sendRefSigAndReturn(false);
        }
    }

    return sendRefSigAndReturn(true);
}

bool PartedCore::mountLV(const LVAction &lvAction)
{
    //判断lv是否存在 不存在退出
    qDebug() << __FUNCTION__ << "mount LV start";
    if (lvAction.m_lvAct != LVM_ACT_LV_MOUNT || !m_lvmInfo.lvInfoExists(lvAction.m_vgName, lvAction.m_lvName)) {
        qDebug() << __FUNCTION__ << "mount LV error:lv not exists or lvact not  LVM_ACT_LV_MOUNT";
        return sendRefSigAndReturn(false);
    }

    //永久挂载
    LVInfo lv = m_lvmInfo.getLVInfo(lvAction.m_vgName, lvAction.m_lvName);
    QString type = Utils::fileSystemTypeToString(lv.m_lvFsType);
    if (!mountDevice(lvAction.m_mountPoint, lv.m_lvPath, lv.m_lvFsType)) {
        return sendRefSigAndReturn(false);
    }
    return   sendRefSigAndReturn(writeFstab(lv.m_mountUuid, lvAction.m_mountPoint, type, true));
}

bool PartedCore::umountLV(const LVAction &lvAction)
{
    //判断lv是否存在 不存在退出
    qDebug() << __FUNCTION__ << "Unmount LV start";
    if (lvAction.m_lvAct != LVM_ACT_LV_UMOUNT || !m_lvmInfo.lvInfoExists(lvAction.m_vgName, lvAction.m_lvName)) {
        qDebug() << __FUNCTION__ << "Unmount LV error:lv not exists or lvact not  LVM_ACT_LV_UMOUNT";
        return sendRefSigAndReturn(false, DISK_SIGNAL_TYPE_UMNT, true, "0");
    }
    //卸载
    LVInfo lv = m_lvmInfo.getLVInfo(lvAction.m_vgName, lvAction.m_lvName);
    if (!umontDevice(lv.m_mountPoints, lv.toMapperPath())) { //卸载挂载点  内部有信号发送 不要重复发送信号
        qDebug() << __FUNCTION__ << "Unmount LV error:lv umount error";
        return false;
    }
    //修改fstab
    writeFstab(lv.m_mountUuid, "", Utils::fileSystemTypeToString(lv.m_lvFsType), false);
    qDebug() << __FUNCTION__ << "Unmount LV end";
    return sendRefSigAndReturn(true, DISK_SIGNAL_TYPE_UMNT, true, "1");
}

bool PartedCore::clearLV(const LVAction &lvAction)
{
    qDebug() << __FUNCTION__ << "clearLV LV start";

    // 判断act是否正确 lv是否存在
    if (!(lvAction.m_lvAct == LVM_ACT_LV_FAST_CLEAR || lvAction.m_lvAct == LVM_ACT_LV_SECURE_CLEAR)
            || !m_lvmInfo.lvInfoExists(lvAction.m_vgName, lvAction.m_lvName)) {
        qDebug() << __FUNCTION__ << "clearLV LV error:lv not exists or lvact not  LVM_ACT_LV_FAST_CLEAR or LVM_ACT_LV_SECURE_CLEAR";
        return sendRefSigAndReturn(false, DISK_SIGNAL_TYPE_CLEAR, false, QString("0:%1").arg(DISK_ERROR::DISK_ERR_DBUS_ARGUMENT));
    }

    //判断需要创建的文件系统是否符合
    bool success = (lvAction.m_lvFs == FS_NTFS || lvAction.m_lvFs == FS_FAT16
                    || lvAction.m_lvFs == FS_FAT32 || lvAction.m_lvFs == FS_EXT2
                    || lvAction.m_lvFs == FS_EXT3 || lvAction.m_lvFs == FS_EXT4);
    LVInfo lv = m_lvmInfo.getLVInfo(lvAction.m_vgName, lvAction.m_lvName);
    if (lv.m_busy || !success) { //被挂载 or 文件系统不支持 退出
        return sendRefSigAndReturn(false, DISK_SIGNAL_TYPE_CLEAR, false, QString("0:%1").arg(DISK_ERROR::DISK_ERR_DBUS_ARGUMENT));
    }


    //关闭加密磁盘
    if (m_LUKSInfo.luksExists(lv.m_lvPath)) {
        LUKS_INFO info = m_LUKSInfo.getLUKS(lv.m_lvPath);
        success = LUKSOperator::removeMapperAndKey(m_LUKSInfo, info);
    }

    if (!success) {
        return sendRefSigAndReturn(false, DISK_SIGNAL_TYPE_CLEAR, false, QString("0:%1").arg(DISK_ERROR::DISK_ERR_DBUS_ARGUMENT));
    }


    //获取文件系统类型
    QString type = Utils::getFileSystemSoftWare(lvAction.m_lvFs);
    switch (lvAction.m_lvAct) {
    case LVM_ACT_LV_SECURE_CLEAR:
        //清除算法
        secuClear(lv.m_lvPath, 0, lv.m_lvLECount - 1, lv.m_LESize, type); //不要加break 如果是安全清除 还是需要创建文件系统的
    case LVM_ACT_LV_FAST_CLEAR: {
        QString tmpPath = QString("/dev/%1/%2").arg(lvAction.m_vgName).arg(lvAction.m_lvName);
        //判断是否加密
        if (lvAction.m_luksFlag == LUKSFlag::IS_CRYPT_LUKS) {
            LUKS_INFO info = getNewLUKSInfo(lvAction);
            //加密失败
            if (!LUKSOperator::encrypt(m_LUKSInfo, info)) {
                return sendRefSigAndReturn(false);
            }
            //解密失败
            if (!LUKSOperator::decrypt(m_LUKSInfo, info)) {
                return sendRefSigAndReturn(false);
            }
            tmpPath = info.m_mapper.m_dmPath;
        }

        //创建文件系统
        if (!createFileSystem(lvAction.m_lvFs, false, tmpPath)) {
            return sendRefSigAndReturn(false, DISK_SIGNAL_TYPE_CLEAR, false, QString("0:%1").arg(DISK_ERROR::DISK_ERR_CREATE_FS_FAILED));
        }
        break;
    }
    default:
        return sendRefSigAndReturn(false, DISK_SIGNAL_TYPE_CLEAR, false, QString("0:%1").arg(DISK_ERROR::DISK_ERR_DBUS_ARGUMENT));
    }

    QString mountPath, devPath;
    //判断是否加密
    if (lvAction.m_luksFlag == LUKSFlag::IS_CRYPT_LUKS) {
        probeDeviceInfo();
        LUKS_INFO info = m_LUKSInfo.getLUKS(lv.m_lvPath);
        //自动挂载 获取挂载文件夹名字
        mountPath = QString("/media/%1/%2").arg(lvAction.m_user).arg(info.m_mapper.m_uuid);
        devPath = info.m_mapper.m_dmPath;
    } else {
        mountPath = QString("/media/%1/%2").arg(lvAction.m_user).arg(lv.m_lvUuid);
        devPath = lv.m_lvPath;
    }
    QPair<bool, QString>pair = tmpMountDevice(mountPath, devPath, lvAction.m_lvFs, lvAction.m_user);
    qDebug() << __FUNCTION__ << "clearLV LV end";
    return sendRefSigAndReturn(pair.first, DISK_SIGNAL_TYPE_CLEAR, pair.first, pair.second);
}

bool PartedCore::deletePVList(QList<PVData> devList)
{
    if (m_workerLVMThread == nullptr) {
        m_workerLVMThread = new QThread();
        m_workerLVMThread->start();
        m_lvmThread.moveToThread(m_workerLVMThread);
    }

    //线程启动信号
    emit deletePVListStart(m_lvmInfo, devList);
    return true;
}

bool PartedCore::delTempMountFile()
{
    QDir dir("/media");
    QDir userPath;
    if (!dir.exists()) {
        return false;
    }
    QStringList mountPath;
    for (int i = 2 ; i < dir.count(); i++) {
        userPath.setPath(QString("/media/%1").arg(dir[i]));
        for (int j = 2; j < userPath.count(); j++) {
            mountPath.append(QString("/media/%1/%2").arg(dir[i]).arg(userPath[j]));
        }
    }
    QMap<QString, Device>::iterator it = m_deviceMap.begin();
    for (; it != m_deviceMap.end() ; it++) {
        for (int i = 0; i < it->m_partitions.size(); i++) {
            if (mountPath.contains(it->m_partitions[i]->getMountPoint())) {
                mountPath.removeOne(it->m_partitions[i]->getMountPoint());
            }
        }
    }

    QDir d;
    char value[1024] = {0};
    for (int i = 0; i < mountPath.size(); i++) {
        getxattr(mountPath[i].toStdString().c_str(), "user.deepin-diskmanager", value, 1024);
        if (QString(value) == "deepin-diskmanager") {
            d.rmdir(mountPath[i]);
        }
    }
    return true;
}

bool PartedCore::format(const QString &fstype, const QString &name)
{
    qDebug() << __FUNCTION__ << "Format Partitione start";
    Partition part = m_curpartition;
    part.m_fstype = Utils::stringToFileSystemType(fstype);
    part.setFilesystemLabel(name);
    bool success = formatPartition(part);
    qDebug() << __FUNCTION__ << "Format Partitione end";
    return success;
}

bool PartedCore::clear(const WipeAction &wipe)
{
    qDebug() << __FUNCTION__ << QString("Clear Partitione start, path: %1").arg(wipe.m_path) ;
    bool success = false;
    FSType fs = Utils::stringToFileSystemType(wipe.m_fstype);
    success = (fs == FS_NTFS || fs == FS_FAT16 || fs == FS_FAT32 || fs == FS_EXT2 || fs == FS_EXT3 || fs == FS_EXT4);
    if (success) {
        if (wipe.m_path.isEmpty() || wipe.m_user.isEmpty()) {
            success = false;
        }

        if (wipe.m_diskType < DISK_TYPE || wipe.m_diskType > PART_TYPE) {
            success = false;
        }

        if (wipe.m_clearType < FAST_TYPE || wipe.m_clearType > GUTMANN_TYPE) {
            success = false;
        }
    }

    if (!success) {
        emit refreshDeviceInfo(DISK_SIGNAL_TYPE_CLEAR, false, QString("0:%1").arg(DISK_ERROR::DISK_ERR_DBUS_ARGUMENT));
        return false;
    }

    //关闭加密磁盘
    if (m_LUKSInfo.luksExists(wipe.m_path)) {
        LUKS_INFO info = m_LUKSInfo.getLUKS(wipe.m_path);
        success = LUKSOperator::removeMapperAndKey(m_LUKSInfo, info);
    }

    if (!success) {
        emit refreshDeviceInfo(DISK_SIGNAL_TYPE_CLEAR, false, QString("0:%1").arg(DISK_ERROR::DISK_ERR_DBUS_ARGUMENT));
        return false;
    }

    m_isClear = true;
    PartitionInfo pInfo;
    QString curDiskType;
    QString curDevicePath;
    QString cmd;
    QString output, errstr;

    if (PART_TYPE == wipe.m_diskType) {
        curDevicePath = m_curpartition.m_devicePath;
        curDiskType = m_deviceMap.value(curDevicePath).m_diskType;

        if (curDiskType == "unrecognized") {
            curDiskType = "gpt";

            Device dev = m_deviceMap.value(curDevicePath);
            success = createPartitionTable(dev.m_path, QString("%1").arg(dev.m_length), QString("%1").arg(dev.m_sectorSize),
                                           curDiskType);
            probeDeviceInfo();
        }

        pInfo.m_fileSystemType = Utils::stringToFileSystemType(wipe.m_fstype);
        pInfo.m_fileSystemLabel = wipe.m_fileSystemLabel ;
        pInfo.m_devicePath = m_curpartition.m_devicePath;
        pInfo.m_sectorSize = m_curpartition.m_sectorSize;
        pInfo.m_sectorStart = m_curpartition.m_sectorStart;
        pInfo.m_fileSystemReadOnly = false;
        if ("unallocated" == wipe.m_path) {
            pInfo.m_insideExtended = false;
            pInfo.m_busy = false;
            pInfo.m_fileSystemReadOnly = false;
            pInfo.m_alignment = ALIGN_MEBIBYTE;
            pInfo.m_type = TYPE_PRIMARY;
            if (0 == pInfo.m_sectorStart) {
                pInfo.m_sectorStart = UEFI_SECTOR;
            }

            Device tmpDev = m_deviceMap.value(pInfo.m_devicePath);

            if (curDiskType == "gpt" && tmpDev.m_length == (m_curpartition.m_sectorEnd + 1)) {
                pInfo.m_sectorEnd = m_curpartition.m_sectorEnd - GPTBACKUP;
            } else {
                pInfo.m_sectorEnd = m_curpartition.m_sectorEnd;
            }
            PartitionVec pVec;
            pVec.push_back(pInfo);
            success = create(pVec);
            probeDeviceInfo();

            Device dev = m_deviceMap.value(curDevicePath);
            for (int i = 0 ; i < dev.m_partitions.size(); i++) {
                Partition *info = dev.m_partitions.at(i);
                if (info->m_sectorStart == pInfo.m_sectorStart && info->m_sectorEnd == pInfo.m_sectorEnd) {
                    m_curpartition = *info;
                    break;
                }
            }
        } else {
            pInfo.m_alignment = m_curpartition.m_alignment;
            pInfo.m_type = m_curpartition.m_type;
            pInfo.m_insideExtended = m_curpartition.m_insideExtended;
            pInfo.m_busy = m_curpartition.m_busy;
            pInfo.m_sectorEnd = m_curpartition.m_sectorEnd;
        }
    } else if (DISK_TYPE == wipe.m_diskType) {
        Device dev = m_deviceMap.value(wipe.m_path);
        curDevicePath = wipe.m_path;
        qDebug() << __FUNCTION__ << "Clear:  createPartitionTable start : " << dev.m_path;
        curDiskType = dev.m_diskType;
        if (curDiskType == "unrecognized") {
            curDiskType = "gpt";
        }
        success = createPartitionTable(dev.m_path, QString("%1").arg(dev.m_length), QString("%1").arg(dev.m_sectorSize), curDiskType);
        probeDeviceInfo();

        PartitionVec pVec;
        pInfo.m_fileSystemType = Utils::stringToFileSystemType(wipe.m_fstype);;
        pInfo.m_fileSystemLabel = wipe.m_fileSystemLabel ;
        pInfo.m_alignment = ALIGN_MEBIBYTE;
        pInfo.m_sectorSize = dev.m_sectorSize;
        pInfo.m_insideExtended = false;
        pInfo.m_busy = false;
        pInfo.m_fileSystemReadOnly = false;
        pInfo.m_devicePath = wipe.m_path;
        pInfo.m_type = TYPE_PRIMARY;
        pInfo.m_sectorStart = UEFI_SECTOR;


        if (curDiskType == "gpt") {
            pInfo.m_sectorEnd = dev.m_length - 34;
        } else {
            pInfo.m_sectorEnd = dev.m_length - 1;
        }

        pVec.push_back(pInfo);
        qDebug() << __FUNCTION__ << "Clear:  createPartition  start : " << pInfo.m_path;
        success = create(pVec);
        probeDeviceInfo();

        Device devTmp = m_deviceMap.value(wipe.m_path);
        if (devTmp.m_partitions.count() >= 1) {
            m_curpartition = *(devTmp.m_partitions[0]);
        }

    }

    //清除
    switch (wipe.m_clearType) {
    case FAST_TYPE:
        qDebug() << __FUNCTION__ << "Clear:  format  start";
        success = format(wipe.m_fstype, wipe.m_fileSystemLabel);
        if (!success) {
            qDebug() << __FUNCTION__ << "format error";
            emit clearMessage(QString("0:%1").arg(DISK_ERROR::DISK_ERR_DELETE_PART_FAILED));
            m_isClear = false;
            return success;
        }
        break;
    case SECURE_TYPE:
        qDebug() << __FUNCTION__ << "Clear:  secuClear  start";
        success = secuClear(m_curpartition.getPath(), m_curpartition.m_sectorStart, m_curpartition.m_sectorEnd, m_curpartition.m_sectorSize, wipe.m_fstype, wipe.m_fileSystemLabel, 1);
        break;
    case DOD_TYPE:
        qDebug() << __FUNCTION__ << "Clear:  secuClear  start";
        success = secuClear(m_curpartition.getPath(), m_curpartition.m_sectorStart, m_curpartition.m_sectorEnd, m_curpartition.m_sectorSize, wipe.m_fstype, wipe.m_fileSystemLabel, 7);
        break;
    case GUTMANN_TYPE:
        qDebug() << __FUNCTION__ << "Clear:  secuClear  start";
        success = secuClear(m_curpartition.getPath(), m_curpartition.m_sectorStart, m_curpartition.m_sectorEnd, m_curpartition.m_sectorSize, wipe.m_fstype, wipe.m_fileSystemLabel, 35);
        break;
    default:
        m_isClear = false;
        return false;
    }



    if (!success) {
        qDebug() << __FUNCTION__ << "secuClear error";
        emit clearMessage(QString("0:%1").arg(DISK_ERROR::DISK_ERR_UPDATE_KERNEL_FAILED));
        m_isClear = false;
        return success;
    }



    // 如果是清理是磁盘时，需要新建分区表，并且新建分区
    if (DISK_TYPE == wipe.m_diskType) {
        Device d = m_deviceMap.value(wipe.m_path);
        qDebug() << __FUNCTION__ << "Clear after:  createPartitionTable  start";
        success = createPartitionTable(d.m_path, QString("%1").arg(d.m_length), QString("%1").arg(d.m_sectorSize), curDiskType);
        probeDeviceInfo();

        //新建分区
        PartitionVec pVec;
        qDebug() << "pInfo end :   " << pInfo.m_sectorEnd;
        pVec.push_back(pInfo);
        qDebug() << __FUNCTION__ << "Clear after:  create Partition  start";
        create(pVec);
    }

    //如果清理是分区，并且清除模式不是快速清除时，执行格式化分区
    if (FAST_TYPE != wipe.m_clearType && PART_TYPE == wipe.m_diskType) {
        //新建分区
        success = format(wipe.m_fstype, wipe.m_fileSystemLabel);
        if (!success) {
            qDebug() << __FUNCTION__ << "format error";
            blockSignals(false);
            emit clearMessage(QString("0:%1").arg(DISK_ERROR::DISK_ERR_DELETE_PART_FAILED));
            m_isClear = false;
            return success;
        }
    }

    probeDeviceInfo();

    //创建完分区后，将当前创建的分区设置为当前选中分区
    if (DISK_TYPE  == wipe.m_diskType) {
        Device d = m_deviceMap.value(wipe.m_path);
        if (d.m_partitions.count() >= 1) {
            m_curpartition = *(d.m_partitions[0]);
        }
    } else {
        Device d = m_deviceMap.value(curDevicePath);
        for (int i = 0 ; i < d.m_partitions.size(); i++) {
            Partition *info = d.m_partitions.at(i);
            if (pInfo.m_sectorStart == info->m_sectorStart && pInfo.m_sectorEnd == info->m_sectorEnd) {
                m_curpartition = *info;
                break;
            }
        }
    }
//   m_curpartition.m_fstype = Utils::stringToFileSystemType(fstype);

    //TODO:

    if (wipe.m_luksFlag == LUKSFlag::NOT_CRYPT_LUKS) {
        //自动挂载
        QFileInfo f;
        //获取挂载文件夹名字，若名字不存在，使用uuid作为名字挂载
        QString mountPath = QString("/media/%1/%2").arg(wipe.m_user).arg(wipe.m_fileSystemLabel);
        if (wipe.m_fileSystemLabel .trimmed().isEmpty()) {
            mountPath = QString("/media/%1/%2").arg(wipe.m_user).arg(FsInfo::getUuid(m_curpartition.getPath()));
        } else {
            f.setFile(mountPath);
            if (f.exists()) {
                mountPath = QString("/media/%1/%2").arg(wipe.m_user).arg(FsInfo::getUuid(m_curpartition.getPath()));
            }
        }

        //设置文件属性，删除时，按照文件属性删除
        if (!createTmpMountDir(mountPath)) {
            success = false;
            m_isClear = false;
            emit refreshDeviceInfo(DISK_SIGNAL_TYPE_CLEAR, true,  QString("0:%1").arg(DISK_ERROR::DISK_ERR_UPDATE_KERNEL_FAILED));
            return success;
        }


        //挂载
        qDebug() << __FUNCTION__ << "Clear after:  mountTemp  start";
        success = mountDevice(mountPath, m_curpartition.getPath(), m_curpartition.m_fstype);
        if (!success) {
            emit refreshDeviceInfo(DISK_SIGNAL_TYPE_CLEAR, true, QString("0:%1").arg(DISK_ERROR::DISK_ERR_UPDATE_KERNEL_FAILED));
            m_isClear = false;
            return success;
        }
        qDebug() << __FUNCTION__ << "clear end";

        //更改属主

        changeOwner(wipe.m_user, mountPath);
        m_isClear = false;
        emit refreshDeviceInfo(DISK_SIGNAL_TYPE_CLEAR, true,  QString("1:0"));
        return success;
    } else {
        Partition p = m_curpartition;
        p.m_dmName = wipe.m_dmName;
        p.m_tokenList = wipe.m_tokenList;
        p.m_decryptStr = wipe.m_decryptStr;
        p.m_luksFlag = wipe.m_luksFlag;
        p.m_cryptCipher = wipe.m_crypt;

        LUKS_INFO info = getNewLUKSInfo(p);
        if (!(LUKSOperator::encrypt(m_LUKSInfo, info)
                && LUKSOperator::decrypt(m_LUKSInfo, info)
                && createFileSystem(info.m_mapper.m_luksFs, info.m_mapper.m_busy, info.m_mapper.m_dmPath))) {
            m_isClear = false;
            return sendRefSigAndReturn(false, DISK_SIGNAL_TYPE_CLEAR, false, QString("0:%1").arg(DISK_ERROR::DISK_ERR_UPDATE_KERNEL_FAILED));
        }

        probeDeviceInfo();

        //挂载
        LUKS_INFO info2 = m_LUKSInfo.getLUKS(m_curpartition.getPath());
        QString mountPath = QString("/media/%1/%2").arg(wipe.m_user).arg(info2.m_mapper.m_uuid);
        QString devPath = info.m_mapper.m_dmPath;
        QPair<bool, QString>pair = tmpMountDevice(mountPath, devPath, info2.m_mapper.m_luksFs, wipe.m_user);
        m_isClear = false;
        qDebug() << __FUNCTION__ << "clearLV LV end";
        return sendRefSigAndReturn(pair.first, DISK_SIGNAL_TYPE_CLEAR, pair.first, pair.second);
    }
}

bool PartedCore::resize(const PartitionInfo &info)
{
    qDebug() << __FUNCTION__ << "Resize Partitione start: " << info.m_devicePath;

//    //对应 Bug 95232, 如果检测到虚拟磁盘扩容的话，重新写一下分区表，就可以修正分区表数据.
    reWritePartition(info.m_devicePath);
//    QString cmd = QString("fdisk -l %1 2>&1").arg(info.m_devicePath);
//    FILE *fd = nullptr;
//    fd = popen(cmd.toStdString().data(), "r");
//    char pb[1024];
//    memset(pb, 0, 1024);

//    if (fd) {
//        while (fgets(pb, 1024, fd) != nullptr) {
//            if (strstr(pb, "will be corrected by write")) {
//                QString cmd_fix = QString("echo w | fdisk %1").arg(info.m_devicePath);

//                FILE *fixfd = popen(cmd_fix.toStdString().data(), "r");
//                if (fixfd) {
//                    fclose(fixfd);
//                }
//                qDebug() << __FUNCTION__ << "createPartition Partition Table Rewrite Done";
//            }
//        }

//        fclose(fd);
//    }

    qDebug() << __FUNCTION__ << "Resize Partitione start";
    Partition newPartition = m_curpartition;
    newPartition.reset(info);
    if (newPartition.getByteLength() < m_curpartition.m_fsLimits.min_size) {
        return sendRefSigAndReturn(false);
    }

    bool success = resize(newPartition);
    emit refreshDeviceInfo();
    qDebug() << __FUNCTION__ << "Resize Partitione end";
    return success;
}

QStringList PartedCore::getallsupportfs()
{
    if (nullptr == m_supportedFileSystems) {
        m_supportedFileSystems = new SupportedFileSystems();
        //Determine file system support capabilities for the first time
        m_supportedFileSystems->findSupportedFilesystems();
    }
    return m_supportedFileSystems->getAllFsName();
}

HardDiskInfo PartedCore::getDeviceHardInfo(const QString &devicepath)
{
    qDebug() << __FUNCTION__ << "Get Device Hard Info Start";
    HardDiskInfo hdinfo;
    if (devicepath.isEmpty()) {
        qDebug() << "disk path is empty";
        return hdinfo;
    }

    DeviceStorage device;
    device.getDiskInfoFromHwinfo(devicepath);

    device.getDiskInfoFromLshw(devicepath);

    device.getDiskInfoFromLsblk(devicepath);

    device.getDiskInfoFromSmartCtl(devicepath);

    hdinfo.m_model = device.m_model;
    hdinfo.m_vendor = device.m_vendor;
    hdinfo.m_mediaType = device.m_mediaType;
    hdinfo.m_size = device.m_size;
    hdinfo.m_rotationRate = device.m_rotationRate;
    hdinfo.m_interface = device.m_interface;
    hdinfo.m_serialNumber = device.m_serialNumber;
    hdinfo.m_version = device.m_version;
    hdinfo.m_capabilities = device.m_capabilities;
    hdinfo.m_description = device.m_description;
    hdinfo.m_powerOnHours = device.m_powerOnHours;
    hdinfo.m_powerCycleCount = device.m_powerCycleCount;
    hdinfo.m_firmwareVersion = device.m_firmwareVersion;
    hdinfo.m_speed = device.m_speed;

    qDebug() << __FUNCTION__ << "Get Device Hard Info end";
    return hdinfo;
}

DeviceInfo PartedCore::getDeviceinfo()
{
    DeviceInfo info;
    return info;
}

DeviceInfoMap PartedCore::getAllDeviceinfo()
{
    return m_inforesult;
}

LVMInfo PartedCore::getAllLVMinfo()
{
    return m_lvmInfo;
}

LUKSMap PartedCore::getAllLUKSinfo()
{
    return m_LUKSInfo;
}

void PartedCore::setCurSelect(const PartitionInfo &info)
{
    auto it = m_deviceMap.find(info.m_devicePath);
    if (it == m_deviceMap.end()) {
        return;
    }

    Device dev = it.value();

    foreach (const Partition *part, dev.m_partitions) {
        if (infoBelongToPartition(*part, info)) {
            m_curpartition = *part;
            return ;
        }
        if (part->m_insideExtended) {//扩展分区  查看是否为逻辑分区
            foreach (const Partition *partlogical, part->m_logicals) {
                if (infoBelongToPartition(*partlogical, info)) {
                    m_curpartition = *partlogical;
                    return;
                }
            }
        }
    }
}

QString PartedCore::getDeviceHardStatus(const QString &devicepath)
{
    qDebug() << __FUNCTION__ << "Get Device Hard Status Start";
    QString status;
    QString devicePath = devicepath;
    if (devicepath.contains("nvme")) {
        QStringList list = devicepath.split("nvme");
        if (list.size() < 2) {
            return status;
        }
        QString str = "";
        for (int i = 0; i < list.at(1).size(); i++) {
            if (list.at(1).at(i) >= "0" && list.at(1).at(i) <= "9") {
                str += list.at(1).at(i);
            } else {
                break;
            }
        }

        devicePath = list.at(0) + "nvme" + str;
//        qDebug() << devicePath << "1111111111111111" << endl;
    }

    QString cmd = QString("smartctl -H %1").arg(devicePath);
    QProcess proc;
    proc.start(cmd);
    proc.waitForFinished(-1);
    QString output = proc.readAllStandardOutput();

    if (output.indexOf("SMART overall-health self-assessment test result:") != -1) {
        QStringList list = output.split("\n");
        for (int i = 0; i < list.size(); i++) {
            if (list.at(i).indexOf("SMART overall-health self-assessment test result:") != -1) {
                status = list.at(i).mid(strlen("SMART overall-health self-assessment test result:"));
                status.remove(QRegExp("^ +\\s*"));
//                qDebug() << __FUNCTION__ << status;
                break;
            }
        }
    } else {
        QString cmd = QString("smartctl -H -d sat %1").arg(devicePath);
        QProcess proc;
        proc.start(cmd);
        proc.waitForFinished(-1);
        QString output = proc.readAllStandardOutput();

        if (output.indexOf("SMART overall-health self-assessment test result:") != -1) {
            QStringList list = output.split("\n");
            for (int i = 0; i < list.size(); i++) {
                if (list.at(i).indexOf("SMART overall-health self-assessment test result:") != -1) {
                    status = list.at(i).mid(strlen("SMART overall-health self-assessment test result:"));
                    status.remove(QRegExp("^ +\\s*"));
//                    qDebug() << __FUNCTION__ << status;
                    break;
                }
            }
        }
    }

    qDebug() << __FUNCTION__ << "Get Device Hard Status End";
    return status;
}

HardDiskStatusInfoList PartedCore::getDeviceHardStatusInfo(const QString &devicepath)
{
    qDebug() << __FUNCTION__ << "Get Device Hard Status Info Start";
    HardDiskStatusInfoList hdsilist;

    QString devicePath = devicepath;
//    QString devicePath = "/dev/nvme12n1";
    if (devicePath.contains("nvme")) {
        //重新拼接硬盘字符串
        QStringList list = devicePath.split("nvme");
        if (list.size() < 2) {
            return hdsilist;
        }
        QString str = "";
        for (int i = 0; i < list.at(1).size(); i++) {
            if (list.at(1).at(i) >= "0" && list.at(1).at(i) <= "9") {
                str += list.at(1).at(i);
            } else {
                break;
            }
        }

        devicePath = list.at(0) + "nvme" + str;

        QString cmd = QString("smartctl -A %1").arg(devicePath);
        QProcess proc;
        proc.start(cmd);
        proc.waitForFinished(-1);
        QString output = proc.readAllStandardOutput();
//        QString output = "smartctl 6.6 2017-11-05 r4594 [x86_64-linux-4.19.0-6-amd64] (local build)\nCopyright (C) 2002-17, Bruce Allen, Christian Franke, www.smartmontools.org\n\n=== START OF SMART DATA SECTION ===\nSMART/Health Information (NVMe Log 0x02, NSID 0xffffffff)\nCritical Warning:                   0x00\nTemperature:                        25 Celsius\nAvailable Spare:                    100%\nAvailable Spare Threshold:          5%\nPercentage Used:                    1%\nData Units Read:                    3,196,293 [1.63 TB]\nData Units Written:                 3,708,861 [1.89 TB]\nHost Read Commands:                 47,399,157\nHost Write Commands:                65,181,192\nController Busy Time:               418\nPower Cycles:                       97\nPower On Hours:                     1,362\nUnsafe Shutdowns:                   44\nMedia and Data Integrity Errors:    0\nError Information Log Entries:      171\nWarning  Comp. Temperature Time:    0\nCritical Comp. Temperature Time:    0\n\n";
        list.clear();
        list = output.split("\n");
        for (int i = 0; i < list.size(); i++) {
            HardDiskStatusInfo hdsinfo;
            if (list.at(i).contains(":")) {
                QStringList slist = list.at(i).split(":");
                if (slist.size() != 2) {
                    break;
                }
                hdsinfo.m_attributeName = slist.at(0);
                hdsinfo.m_rawValue = slist.at(1).trimmed();
            } else {
                continue;
            }
            hdsilist.append(hdsinfo);
        }
    } else {

        QString cmd = QString("smartctl -A %1").arg(devicepath);
        QProcess proc;
        proc.start(cmd);
        proc.waitForFinished(-1);
        QString output = proc.readAllStandardOutput();

        if (output.contains("ID# ATTRIBUTE_NAME          FLAG     VALUE WORST THRESH TYPE      UPDATED  WHEN_FAILED RAW_VALUE")) {
            QStringList list = output.split("\n");
            int n = list.indexOf("ID# ATTRIBUTE_NAME          FLAG     VALUE WORST THRESH TYPE      UPDATED  WHEN_FAILED RAW_VALUE");
            for (int i = n + 1; i < list.size(); i++) {
                HardDiskStatusInfo hdsinfo;
                QString statusInfo = list.at(i);


                QStringList slist = statusInfo.split(' ');
                slist.removeAll("");

                if (slist.size() == 0) {
                    break;
                }

                if (list.size() >= 10) {
                    hdsinfo.m_id = slist.at(0);
                    hdsinfo.m_attributeName = slist.at(1);
                    hdsinfo.m_flag = slist.at(2);
                    hdsinfo.m_value = slist.at(3);
                    hdsinfo.m_worst = slist.at(4);
                    hdsinfo.m_thresh = slist.at(5);
                    hdsinfo.m_type = slist.at(6);
                    hdsinfo.m_updated = slist.at(7);
                    hdsinfo.m_whenFailed = slist.at(8);
                    for (int k = 9; k < slist.size(); k++) {
                        hdsinfo.m_rawValue += slist.at(k);
                    }
                }

                hdsilist.append(hdsinfo);
            }
        } else {
            QString cmd = QString("smartctl -A -d sat %1").arg(devicepath);
            QProcess proc;
            proc.start(cmd);
            proc.waitForFinished(-1);
            QString output = proc.readAllStandardOutput();
            if (output.contains("ID# ATTRIBUTE_NAME          FLAG     VALUE WORST THRESH TYPE      UPDATED  WHEN_FAILED RAW_VALUE")) {
                QStringList list = output.split("\n");
                int n = list.indexOf("ID# ATTRIBUTE_NAME          FLAG     VALUE WORST THRESH TYPE      UPDATED  WHEN_FAILED RAW_VALUE");
                for (int i = n + 1; i < list.size(); i++) {
                    HardDiskStatusInfo hdsinfo;
                    QString statusInfo = list.at(i);
                    QStringList slist = statusInfo.split(' ');
                    slist.removeAll("");

                    if (slist.size() == 0) {
                        break;
                    }

                    if (list.size() >= 10) {
                        hdsinfo.m_id = slist.at(0);
                        hdsinfo.m_attributeName = slist.at(1);
                        hdsinfo.m_flag = slist.at(2);
                        hdsinfo.m_value = slist.at(3);
                        hdsinfo.m_worst = slist.at(4);
                        hdsinfo.m_thresh = slist.at(5);
                        hdsinfo.m_type = slist.at(6);
                        hdsinfo.m_updated = slist.at(7);
                        hdsinfo.m_whenFailed = slist.at(8);
                        for (int k = 9; k < slist.size(); k++) {
                            hdsinfo.m_rawValue += slist.at(k);
                        }
                    }

                    hdsilist.append(hdsinfo);
                }
            }
        }
    }
    qDebug() << __FUNCTION__ << "Get Device Hard Status Info end";
    return hdsilist;
}
bool PartedCore::deletePartition()
{
    qDebug() << __FUNCTION__ << "Delete Partition start";
    PedPartition *ped = nullptr;
    PedDevice *lpDevice = nullptr;
    PedDisk *lpDisk = nullptr;

    QString parttitionPath = m_curpartition.getPath();
    QString devicePath = m_curpartition.m_devicePath;

    //close luksMapper
    if (m_LUKSInfo.luksExists(devicePath)) {
        LUKS_INFO info = m_LUKSInfo.getLUKS(devicePath);
        if (!LUKSOperator::removeMapperAndKey(m_LUKSInfo, info)) {
            qDebug() << __FUNCTION__ << "Close luks Mapper failed";
            return sendRefSigAndReturn(false, m_isClear ? DISK_SIGNAL_TYPE_CLEAR : DISK_SIGNAL_TYPE_DEL,
                                       true, QString("0:%1").arg(DISK_ERROR::DISK_ERR_PART_INFO));
        }
    }


    bool success = true;
    if (m_LUKSInfo.luksExists(parttitionPath)) {
        LUKS_INFO info = m_LUKSInfo.getLUKS(parttitionPath);

        success = LUKSOperator::removeMapperAndKey(m_LUKSInfo, info);
    }

    if (!getDeviceAndDisk(devicePath, lpDevice, lpDisk) || !success) {
        qDebug() << __FUNCTION__ << "Delete Partition get device and disk failed";

        return sendRefSigAndReturn(false, m_isClear ? DISK_SIGNAL_TYPE_CLEAR : DISK_SIGNAL_TYPE_DEL,
                                   true, QString("0:%1").arg(DISK_ERROR::DISK_ERR_DISK_INFO));
    }

    QStringList list;

    for (int i = parttitionPath.size() - 1; i != 0; i--) {
        if (parttitionPath.at(i) >= '0' && parttitionPath.at(i) <= '9') {
            list.insert(0, parttitionPath.at(i));
        } else {
            break;
        }
    }

    int num = list.join("").toInt();
    ped = ped_disk_get_partition(lpDisk, num);

    if (ped == nullptr) {
        qDebug() << __FUNCTION__ << "Delete Partition Get Partition failed";
        return sendRefSigAndReturn(false, m_isClear ? DISK_SIGNAL_TYPE_CLEAR : DISK_SIGNAL_TYPE_DEL,
                                   true, QString("0:%1").arg(DISK_ERROR::DISK_ERR_PART_INFO));
    }

    int i = ped_disk_delete_partition(lpDisk, ped);

    if (i == 0) {
        qDebug() << __FUNCTION__ << "Delete Partition failed";
        return sendRefSigAndReturn(false, m_isClear ? DISK_SIGNAL_TYPE_CLEAR : DISK_SIGNAL_TYPE_DEL,
                                   true, QString("0:%1").arg(DISK_ERROR::DISK_ERR_DELETE_PART_FAILED));
    }

    if (!commit(lpDisk)) {
        qDebug() << __FUNCTION__ << "Delete Partition commit failed";
        return sendRefSigAndReturn(false, m_isClear ? DISK_SIGNAL_TYPE_CLEAR : DISK_SIGNAL_TYPE_DEL,
                                   true, QString("0:%1").arg(DISK_ERROR::DISK_ERR_UPDATE_KERNEL_FAILED));
    }

    destroyDeviceAndDisk(lpDevice, lpDisk);
    qDebug() << __FUNCTION__ << "Delete Partition end";
    return sendRefSigAndReturn(true, DISK_SIGNAL_TYPE_DEL, true, "1:0");
}

bool PartedCore::hidePartition()
{
    qDebug() << __FUNCTION__ << "Hide Partition start";
//ENV{ID_FS_UUID}==\"1ee3b4c6-1c69-46b9-9656-8c534ffd4f43\", ENV{UDISKS_IGNORE}=\"1\"\n
    getPartitionHiddenFlag();
    if (m_hiddenPartition.indexOf(m_curpartition.m_uuid) != -1) {
        emit refreshDeviceInfo();
        emit hidePartitionInfo("0");
        return false;
    }
    QFile file("/etc/udev/rules.d/80-udisks2.rules");

    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) { //打开指定文件
        qDebug() << __FUNCTION__ << "Permanent mount open file error";
        emit refreshDeviceInfo();
        emit hidePartitionInfo("0");
        return false;
    } else {
        QTextStream out(&file);
        out << QString("ENV{ID_FS_UUID}==\"%1\", ENV{UDISKS_IGNORE}=\"1\"").arg(m_curpartition.m_uuid) << endl;
    }

    qDebug() << __FUNCTION__ << "Hide Partition end";
    QProcess proc;
    proc.start("udevadm control --reload");
    proc.waitForFinished(-1);
    emit refreshDeviceInfo();
    emit hidePartitionInfo("1");
    return true;
}

bool PartedCore::showPartition()
{
    qDebug() << __FUNCTION__ << "Show Partition start";
    getPartitionHiddenFlag();
    if (m_hiddenPartition.indexOf(m_curpartition.m_uuid) == -1) {
        emit refreshDeviceInfo();
        emit showPartitionInfo("0");
        return false;
    }

    QFile file("/etc/udev/rules.d/80-udisks2.rules");
    QStringList list;

    if (!file.open(QIODevice::ReadOnly)) { //打开指定文件
        qDebug() << __FUNCTION__ << "Permanent unmount open file error";
        emit refreshDeviceInfo();
        emit showPartitionInfo("0");
        return false;
    } else {
        QStringList list;
        while (!file.atEnd()) {
            QByteArray line = file.readLine();//获取数据
            QString str = line;

            if (str.contains(m_curpartition.m_uuid)) {
                continue;
            }
            list << str;
        }
        file.close();
        if (file.open(QIODevice::ReadWrite | QIODevice::Truncate)) {
            QTextStream out(&file);
            for (int i = 0; i < list.count(); i++) {
                out << list.at(i);
                out.flush();
            }
            file.close();
        }
    }

    QProcess proc;
    proc.start("udevadm control --reload");
    proc.waitForFinished(-1);
    emit refreshDeviceInfo();
    emit showPartitionInfo("1");

    qDebug() << __FUNCTION__ << "Show Partition end";
    return true;

}

int PartedCore::getPartitionHiddenFlag()
{
    qDebug() << __FUNCTION__ << "Get Partition Hidden Flag start";

    m_hiddenPartition.clear();
    QFile file("/etc/udev/rules.d/80-udisks2.rules");
    QStringList list;

    if (!file.open(QIODevice::ReadOnly)) { //打开指定文件
        qDebug() << __FUNCTION__ << "Permanent mount open file error";
        file.close();
        return -1;
    } else {
        m_hiddenPartition = file.readAll();
//        qDebug() << m_hiddenPartition;
    }
    file.close();
    qDebug() << __FUNCTION__ << "Get Partition Hidden Flag end";
    return 0;
}

bool PartedCore::checkBadBlocks(const QString &devicePath, int blockStart, int blockEnd, int checkConut, int checkSize, int flag)
{
    if (m_workerCheckThread == nullptr) {
        m_workerCheckThread = new QThread();
        m_workerCheckThread->start();
        m_checkThread.moveToThread(m_workerCheckThread);
//      qDebug() << QThread::currentThreadId() << 1111111111111 << endl;
    }

    m_checkThread.setStopFlag(flag);
    if (flag == 1 || flag == 3) {
        m_checkThread.setCountInfo(devicePath, blockStart, blockEnd, checkConut, checkSize);
        emit checkBadBlocksRunCountStart();
    }

    return true;
}

bool PartedCore::checkBadBlocks(const QString &devicePath, int blockStart, int blockEnd, QString checkTime, int checkSize, int flag)
{
    if (m_workerCheckThread == nullptr) {
        m_workerCheckThread = new QThread();
        m_workerCheckThread->start();
        m_checkThread.moveToThread(m_workerCheckThread);
    }

    m_checkThread.setStopFlag(flag);
    if (flag == 1 || flag == 3) {
        m_checkThread.setTimeInfo(devicePath, blockStart, blockEnd, checkTime, checkSize);
        emit checkBadBlocksRunTimeStart();
    }

    return true;
}

bool PartedCore::fixBadBlocks(const QString &devicePath, QStringList badBlocksList, int checkSize, int flag)
{
    if (m_workerFixThread == nullptr) {
        m_workerFixThread = new QThread();
        m_workerFixThread->start();
        m_fixthread.moveToThread(m_workerFixThread);
    }


    /*flag
            1 start
            2 stop
            3 continue
    */

    m_fixthread.setStopFlag(flag);
    if (flag == 1 || flag == 3) {
        m_fixthread.setFixBadBlocksInfo(devicePath, badBlocksList, checkSize);
        emit fixBadBlocksStart();
    }

    return true;
}

bool PartedCore::detectionPartitionTableError(const QString &devicePath)
{
    qDebug() << __FUNCTION__ << "Detection Partition Table Error start";

    struct stat fileStat;
    stat(devicePath.toStdString().c_str(), &fileStat);
    if (!S_ISBLK(fileStat.st_mode)) {
        qDebug() << __FUNCTION__ << QString("%1 is not blk file").arg(devicePath);
        return false;
    }

    bool needRewrite = gptIsExpanded(devicePath);
    if (needRewrite) {
        QString outPutFix, errorFix;
        QString cmdFix = QString("echo w | fdisk %1").arg(devicePath);
        Utils::executWithPipeCmd(cmdFix, outPutFix, errorFix);
        qDebug() << __FUNCTION__ << "createPartition Partition Table Rewrite Done";
        return true;
    }

    QString outPut, error, outPutError;
    QStringList argList;
    argList << "-l" << devicePath << "2>&1";
    int ret = Utils::executWithErrorCmd("fdisk", argList, outPut, outPutError, error);
    if (ret != 0) {
        qDebug() << __FUNCTION__ << "Detection Partition Table Error order error";
        return false;
    }

    QStringList outPulList = outPut.split("\n");
    for (int i = 0; i < outPulList.size(); i++) {
        if (strstr(outPulList[i].toStdString().c_str(), "Partition table entries are not in disk order") != nullptr) {
            return true;
        }
    }
    qDebug() << __FUNCTION__ << "Detection Partition Table Error end";
    return false;

//    QString cmd = QString("fdisk -l %1 2>&1").arg(devicePath);
//    FILE *fd = nullptr;
//    fd = popen(cmd.toStdString().data(), "r");
//    char pb[1024];
//    memset(pb, 0, 1024);

//    if (fd == nullptr) {
//        qDebug() << __FUNCTION__ << "Detection Partition Table Error order error";
//        return false;
//    }

//    while (fgets(pb, 1024, fd) != nullptr) {
//        if (strstr(pb, "Partition table entries are not in disk order") != nullptr) {
//            return true;
//        }
//        if (nullptr != strstr(pb, "will be corrected by write")) {
//            //QString output, errstr;
//            QString cmd_fix = QString("echo w | fdisk %1").arg(devicePath);

//            FILE *fixfd = popen(cmd_fix.toStdString().data(), "r");
//            qDebug() << __FUNCTION__ << "Detection Partition Table Rewrite Done";
//            if (fixfd) {
//                fclose(fixfd);
//            }

//            return true;
//        }
//    }

//    fclose(fd);
//    qDebug() << __FUNCTION__ << "Detection Partition Table Error end";
//    return false;
}

bool PartedCore::updateUsb()
{
    qDebug() << __FUNCTION__ << "USB add update start";

    //sleep(5);
    emit usbUpdated();

    autoMount();

    qDebug() << __FUNCTION__ << "USB add update end";
    return true;
}

bool PartedCore::updateUsbRemove()
{
    qDebug() << __FUNCTION__ << "USB add update remove";

    //emit usbUpdated();
    emit refreshDeviceInfo(DISK_SIGNAL_USBUPDATE);

    autoUmount();

    qDebug() << __FUNCTION__ << "USB add update end";
    return true;
}

void PartedCore::refreshFunc()
{
    qDebug() << __FUNCTION__ << "refreshFunc start";
    emit refreshDeviceInfo();
    qDebug() << __FUNCTION__ << "refreshFunc end";
}

void PartedCore::autoMount()
{
    //因为永久挂载的原因需要先执行mount -a让系统文件挂载生效
    qDebug() << __FUNCTION__ << "solt automount start";
    QString output, errstr;
    QString cmd = QString("mount -a");
    int exitcode = Utils::executCmd(cmd, output, errstr);

    if (exitcode != 0) {
//        qDebug() << __FUNCTION__ << output;
    }

    emit refreshDeviceInfo(DISK_SIGNAL_TYPE_AUTOMNT);

    qDebug() << __FUNCTION__ << "solt automount end";
}

void PartedCore::autoUmount()
{
    qDebug() << __FUNCTION__ << "autoUmount start";
    QStringList deviceList;

    for (auto it = m_inforesult.begin(); it != m_inforesult.end(); it++) {
        deviceList << it.key();
    }
    QString outPut, error;
    QString cmd = QString("df");
    int ret = Utils::executCmd(cmd, outPut, error);
    if (ret != 0) {
        qDebug() << __FUNCTION__ << "Detection Partition Table Error order error";
        return;
    }
    QStringList outPutList = outPut.split("\n");
    for (int i = 0; i < outPutList.size(); i++) {
        QStringList dfList = outPutList[i].split(" ");
        if (deviceList.indexOf(dfList.at(0).left(dfList.at(0).size() - 1)) == -1 && dfList.at(0).contains("/dev/")) {
            QStringList arg;
            arg << "-v" << dfList.last();
            QString output, errstr;
            int exitcode = Utils::executeCmdWithArtList("umount", arg, output, errstr);
            if (exitcode != 0) {
                qDebug() << __FUNCTION__ << "卸载挂载点失败";
            }
        }
    }
    qDebug() << __FUNCTION__ << "autoUmount end";


//    QString cmd = QString("df");
//    FILE *fd = nullptr;
//    fd = popen(cmd.toStdString().data(), "r");
//    char pb[1024];
//    memset(pb, 0, 1024);

//    while (fgets(pb, 1024, fd) != nullptr) {
//        QString dfBuf = pb;
//        QStringList dfList = dfBuf.split(" ");
//        if (deviceList.indexOf(dfList.at(0).left(dfList.at(0).size()-1)) == -1 && dfList.at(0).contains("/dev/")) {
//            cmd = QString("umount -v %1").arg(dfList.last());
//            QString output, errstr;
//            int exitcode = Utils::executCmd(cmd, output, errstr);
//            if (exitcode != 0) {
//                qDebug() << __FUNCTION__ << "卸载挂载点失败";
//            }
//        }
//    }
//    qDebug() << __FUNCTION__ << "autoUmount end";
}

void PartedCore::syncDeviceInfo(/*const QMap<QString, Device> deviceMap, */const DeviceInfoMap inforesult, const LVMInfo lvmInfo, const LUKSMap &luks)
{
    qDebug() << "syncDeviceInfo finally!";
    //m_deviceMap = deviceMap;
    m_deviceMap = m_probeThread.getDeviceMap();
    m_inforesult = inforesult;
    m_lvmInfo = lvmInfo;
    m_LUKSInfo = luks;
    emit updateLUKSInfo(m_LUKSInfo);
    emit updateDeviceInfo(m_inforesult, m_lvmInfo);
}

bool PartedCore::createPartitionTable(const QString &devicePath, const QString &length, const QString &sectorSize, const QString &diskLabel)
{
//    Glib::ustring device_path = device.get_path();

    // FIXME: Should call file system specific removal actions
    // (to remove LVM2 PVs before deleting the partitions).

//#ifdef ENABLE_LOOP_DELETE_OLD_PTNS_WORKAROUND
    // When creating a "loop" table with libparted 2.0 to 3.0 inclusive, it doesn't
    // inform the kernel to delete old partitions so as a consequence blkid's cache
    // becomes stale and it won't report a file system subsequently created on the
    // whole disk device.  Create a GPT first to use that code in libparted to delete
    // any old partitions.  Fixed in parted 3.1 by commit:
    //     f5c909c0cd50ed52a48dae6d35907dc08b137e88
    //     libparted: remove has_partitions check to allow loopback partitions
//    if ( disklabel == "loop" )
//        newDiskLabel( device_path, "gpt", false );
//#endif

    // Ensure that any previous whole disk device file system can't be recognised by
    // libparted in preference to the "loop" partition table signature, or by blkid in
    // preference to any partition table about to be written.
//    OperationDetail dummy_od;
    Sector deviceLength = length.toLongLong();
    Sector deviceSectorSize = sectorSize.toLong();
    Partition tempPartition;
    tempPartition.setUnpartitioned(devicePath,
                                   "",
                                   FS_UNALLOCATED,
                                   deviceLength,
                                   deviceSectorSize,
                                   false);
    eraseFilesystemSignatures(tempPartition);

    bool flag = newDiskLabel(devicePath, diskLabel);

    if (!m_isClear) {
        emit refreshDeviceInfo(DISK_SIGNAL_TYPE_CREATE_TABLE, flag, "");
    }

    //emit createTableMessage(flag);

    return flag;
}

bool PartedCore::newDiskLabel(const QString &devicePath, const QString &diskLabel)
{
    bool returnValue = false;

    PedDevice *lpDevice = nullptr;
    PedDisk *lpDisk = nullptr;
    if (getDevice(devicePath, lpDevice)) {
        PedDiskType *type = nullptr;
        type = ped_disk_type_get(diskLabel.toStdString().c_str());

        if (type) {
            lpDisk = ped_disk_new_fresh(lpDevice, type);
            returnValue = commit(lpDisk) ;
        }

        destroyDeviceAndDisk(lpDevice, lpDisk) ;
    }

//#ifndef USE_LIBPARTED_DMRAID
//    //delete and recreate disk entries if dmraid
//    DMRaid dmraid ;
//    if ( recreate_dmraid_devs && return_value && dmraid.is_dmraid_device( device_path ) )
//    {
//        dmraid .purge_dev_map_entries( device_path ) ;
//        dmraid .create_dev_map_entries( device_path ) ;
//    }
//#endif

    return returnValue;
}

bool PartedCore::gptIsExpanded(const QString &devicePath)
{
    qDebug() << __FUNCTION__ << "gptIsExpanded called";

    /* GPT 与 MBR 分区格式的介绍可以参考下面的链接：
     * https://www.cnblogs.com/cwcheng/p/11270774.html
     * 简单来说，如果MBR内容中，偏移量为0x1c2处的字节内容为 0xee,则表示当前磁盘采用GPT分区表。
    */
    int offset = 0x1c2;
    uint8_t phdr[1024] = {0};

    int fd = ::open(devicePath.toLatin1().data(), O_RDONLY);
    if (fd < 0) {
        return false;
    }

    int ret = ::read(fd, phdr, 1024);
    ::close(fd);
    if (ret < 0) {
        return false;
    }

    if (0xee == phdr[offset]) {
        gpt_header_t *gpt = (gpt_header_t *)(phdr + 512);

        for (auto it = m_deviceMap.begin(); it != m_deviceMap.end(); it++) {
            if (it.key() == devicePath) {
                Device dev = it.value();
                qDebug() << __FUNCTION__ << " CHEN Enum device : \t " << dev.m_length << " " << dev.m_sectors << " " << dev.m_heads;
                if (gpt->alternative_lba != (dev.m_length - 1)) {
                    return true;
                }
            }
        }

    }
    return false;
}

bool PartedCore::mountDevice(const QString &mountpath, const QString devPath, const FSType &fsType)
{
    qDebug() << __FUNCTION__ << "Mount start";
    if (mountpath.isEmpty() || devPath.isEmpty() || fsType == FSType::FS_UNKNOWN) {
        qDebug() << __FUNCTION__ << "Mount argument error";
        return false;
    }

    QString output, errstr;
    QString cmd ;
    //vfat 1051系统上vfat格式不指定utf8挂载 通过文件管理器右键菜单新建文件夹会乱码  导致创建错误 为了规避该问题加上utf8属性
    if (fsType == FSType::FS_FAT32 || fsType == FSType::FS_FAT16) {
        cmd = QString("mount -v %1 %2 -o dmask=000,fmask=111,utf8").arg(devPath).arg(mountpath);
    } else if (fsType == FSType::FS_HFS) {
        cmd = QString("mount -v %1 %2 -o dir_umask=000,file_umask=111").arg(devPath).arg(mountpath);
    } else {
        cmd = QString("mount -v %1 %2").arg(devPath).arg(mountpath);
    }

    int exitcode = Utils::executCmd(cmd, output, errstr);
    if (exitcode != 0) {
        qDebug() << __FUNCTION__ << "Mount error";
        return false;
    }
    qDebug() << __FUNCTION__ << "Mount end";
    return true;

}

bool PartedCore::umontDevice(QVector<QString> mountPoints, QString devPath)
{
    if (devPath.isEmpty()) {
        devPath = m_curpartition.getPath();
    }

    QString output, errstr;
    for (QString path : mountPoints) {
        QStringList arg;
        arg << "-v" << path;
        int exitcode = Utils::executeCmdWithArtList("umount", arg, output, errstr);
        if (0 != exitcode) {
            Utils::executCmd("df", output, errstr);
            return output.contains(devPath) ? sendRefSigAndReturn(false, DISK_SIGNAL_TYPE_UMNT, true, "0")
                   : sendRefSigAndReturn(true, DISK_SIGNAL_TYPE_UMNT, true, "1");
        }
    }
    return true;
}

bool PartedCore::writeFstab(const QString &uuid, const QString &mountpath, const QString &type, bool isMount)
{
    //写入fstab 永久挂载
    qDebug() << __FUNCTION__ << "Write fstab start";
    if (uuid.isEmpty() || (mountpath.isEmpty() && isMount) || type.isEmpty()) { //非挂载 不需要挂载点 可以传空值
        qDebug() << __FUNCTION__ << "Write fstabt argument error";
        return false;
    }

    QFile file("/etc/fstab");
    QStringList list;

    // open fstab
    if (!file.open(QIODevice::ReadOnly)) { //打开指定文件
        qDebug() << __FUNCTION__ << "Write fstabt: open file error";
        return false;
    }

    //此处修改：因为mount读取/etc/fstab配置文件 开机自动挂载  而fat32或fat16 mount不识别，所以要改成vfat
    QString type2 = (type.contains("fat32") || type.contains("fat16")) ? QString("vfat") : type;

    // read fstab
    bool findflag = false; //目前默认只改第一个发现的uuid findflag 标志位：是否已经查找到uuid
    while (!file.atEnd()) {
        QByteArray line = file.readLine();//获取数据
        QString str = line;
        if (isMount) {
            //vfat 1051系统上vfat格式不指定utf8挂载 通过文件管理器右键菜单新建文件夹会乱码  导致创建错误 为了规避该问题加上utf8属性
            QString mountStr = (type2 == "vfat") ? QString("UUID=%1 %2 %3 defaults,nofail,utf8,dmask=000,fmask=111 0 0\n").arg(uuid).arg(mountpath).arg(type2)
                               : QString("UUID=%1 %2 %3 defaults,nofail 0 0\n").arg(uuid).arg(mountpath).arg(type2);

            if (str.contains(uuid) && !findflag) { //首次查找到uuid
                findflag = true;
                list << mountStr;
                continue;
            } else if (file.atEnd() && !findflag) { //查找到结尾且没有查找到uuid
                list << str << mountStr;
                break;
            }
            list << str;
        } else {
            if (!str.contains(uuid)) {
                list << str;
            }
        }
    }
    file.close();

    //write fstab
    if (!file.open(QIODevice::ReadWrite | QIODevice::Truncate)) {
        qDebug() << __FUNCTION__ << "Write fstabt: open file error";
        return false;
    }

    QTextStream out(&file);
    for (int i = 0; i < list.count(); i++) {
        out << list.at(i);
    }
    out.flush();
    file.close();
    qDebug() << __FUNCTION__ << "Write fstabt end";
    return true;
}

bool PartedCore::createTmpMountDir(const  QString &mountPath)
{
    //自动挂载 获取挂载文件夹名字
    QDir dir;
    if (!dir.exists(mountPath)) {
        if (!dir.mkdir(mountPath)) {
            return false;
        }
    }

    //设置文件属性，删除时，按照文件属性删除
    const char *v = "deepin-diskmanager";
    return setxattr(mountPath.toStdString().c_str(), "user.deepin-diskmanager", v, strlen(v), 0) == 0;
}

bool PartedCore::changeOwner(const QString &user, const QString &path)
{
    struct passwd *psInfo;
    psInfo = getpwnam(user.toStdString().c_str());
    if (psInfo == nullptr) {
        return false;
    }
    //todo 暂时先不判断返回值 此处在fat ntfs文件系统有问题
    chown(path.toStdString().c_str(), psInfo->pw_uid, psInfo->pw_gid);
    return true;
}

QPair<bool, QString> PartedCore::tmpMountDevice(const QString &mountpath, const QString devPath, const FSType &fsType, const QString &userName)
{
    //自动挂载 获取挂载文件夹名字
    if (!createTmpMountDir(mountpath)) {
        return QPair<bool, QString>(false, QString("0:%1").arg(DISK_ERROR::DISK_ERR_CREATE_MOUNTDIR_FAILED));
    }

    //挂载
    if (!mountDevice(mountpath, devPath, fsType)) {
        return QPair<bool, QString>(false, QString("0:%1").arg(DISK_ERROR::DISK_ERR_MOUNT_FAILED));
    }

    //更改属主
    if (!changeOwner(userName, mountpath)) {
        return QPair<bool, QString>(false, QString("0:%1").arg(DISK_ERROR::DISK_ERR_CHOWN_FAILED));
    }
    return QPair<bool, QString>(true, QString("1:0"));
}



int PartedCore::test()
{


    return 1;
}

} // namespace DiskManager
