// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "common/singleton.h"
#include "core/file_sys/fs.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/file_system.h"
#include "core/libraries/libs.h"
#include "libkernel.h"

namespace Libraries::Kernel {

std::vector<Core::FileSys::DirEntry> GetDirectoryEntries(const std::string& path) {
    std::vector<Core::FileSys::DirEntry> files;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        auto& dir_entry = files.emplace_back();
        dir_entry.name = entry.path().filename().string();
        dir_entry.isFile = !std::filesystem::is_directory(entry.path().string());
    }

    return files;
}

int PS4_SYSV_ABI sceKernelOpen(const char* path, int flags, u16 mode) {
    LOG_INFO(Kernel_Fs, "path = {} flags = {:#x} mode = {}", path, flags, mode);
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();

    bool read = (flags & 0x3) == ORBIS_KERNEL_O_RDONLY;
    bool write = (flags & 0x3) == ORBIS_KERNEL_O_WRONLY;
    bool rdwr = (flags & 0x3) == ORBIS_KERNEL_O_RDWR;

    bool nonblock = (flags & ORBIS_KERNEL_O_NONBLOCK) != 0;
    bool append = (flags & ORBIS_KERNEL_O_APPEND) != 0;
    bool fsync = (flags & ORBIS_KERNEL_O_FSYNC) != 0;
    bool sync = (flags & ORBIS_KERNEL_O_SYNC) != 0;
    bool create = (flags & ORBIS_KERNEL_O_CREAT) != 0;
    bool truncate = (flags & ORBIS_KERNEL_O_TRUNC) != 0;
    bool excl = (flags & ORBIS_KERNEL_O_EXCL) != 0;
    bool dsync = (flags & ORBIS_KERNEL_O_DSYNC) != 0;
    bool direct = (flags & ORBIS_KERNEL_O_DIRECT) != 0;
    bool directory = (flags & ORBIS_KERNEL_O_DIRECTORY) != 0;

    if (std::string_view{path} == "/dev/console") {
        return 2000;
    }
    if (std::string_view{path} == "/dev/deci_tty6") {
        return 2001;
    }
    if (std::string_view{path} == "/dev/stdout") {
        return 2002;
    }
    u32 handle = h->CreateHandle();
    auto* file = h->GetFile(handle);
    if (directory) {
        file->is_directory = true;
        file->m_guest_name = path;
        file->m_host_name = mnt->GetHostDirectory(file->m_guest_name);
        if (!std::filesystem::is_directory(file->m_host_name)) { // directory doesn't exist
            h->DeleteHandle(handle);
            return ORBIS_KERNEL_ERROR_ENOTDIR;
        } else {
            if (create) {
                return handle; // dir already exists
            } else {
                file->dirents = GetDirectoryEntries(file->m_host_name);
                file->dirents_index = 0;
            }
        }
    } else {
        file->m_guest_name = path;
        file->m_host_name = mnt->GetHostFile(file->m_guest_name);
        if (read) {
            file->f.Open(file->m_host_name, Common::FS::FileAccessMode::Read);
        } else if (write && create && truncate) {
            file->f.Open(file->m_host_name, Common::FS::FileAccessMode::Write);
        } else if (write && create && append) { // CUSA04729 (appends app0/shaderlist.txt)
            file->f.Open(file->m_host_name, Common::FS::FileAccessMode::Append);
        } else if (rdwr) {
            if (create) { // Create an empty file first.
                Common::FS::IOFile out(file->m_host_name, Common::FS::FileAccessMode::Write);
            }
            // RW, then scekernelWrite is called and savedata is written just fine now.
            file->f.Open(file->m_host_name, Common::FS::FileAccessMode::ReadWrite);
        } else {
            UNREACHABLE();
        }
        if (!file->f.IsOpen()) {
            h->DeleteHandle(handle);
            if (create) {
                return ORBIS_KERNEL_ERROR_EACCES;
            } else {
                return ORBIS_KERNEL_ERROR_ENOENT;
            }
        }
    }
    file->is_opened = true;
    return handle;
}

int PS4_SYSV_ABI posix_open(const char* path, int flags, /* SceKernelMode*/ u16 mode) {
    LOG_INFO(Kernel_Fs, "posix open redirect to sceKernelOpen");
    int result = sceKernelOpen(path, flags, mode);
    // Posix calls different only for their return values
    if (result < 0) {
        ErrSceToPosix(result);
        return -1;
    }
    return result;
}

int PS4_SYSV_ABI sceKernelClose(int d) {
    if (d < 3) { // d probably hold an error code
        return ORBIS_KERNEL_ERROR_EPERM;
    }
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(d);
    if (file == nullptr) {
        return SCE_KERNEL_ERROR_EBADF;
    }
    if (!file->is_directory) {
        file->f.Close();
    }
    file->is_opened = false;
    LOG_INFO(Kernel_Fs, "Closing {}", file->m_guest_name);
    h->DeleteHandle(d);
    return SCE_OK;
}

int PS4_SYSV_ABI posix_close(int d) {
    int result = sceKernelClose(d);
    if (result < 0) {
        LOG_ERROR(Kernel_Pthread, "posix_close: error = {}", result);
        ErrSceToPosix(result);
        return -1;
    }
    return result;
    return ORBIS_OK;
}

size_t PS4_SYSV_ABI sceKernelWrite(int d, const void* buf, size_t nbytes) {
    if (d <= 2) { // stdin,stdout,stderr
        char* str = strdup((const char*)buf);
        if (str[nbytes - 1] == '\n')
            str[nbytes - 1] = 0;
        LOG_INFO(Tty, "{}", str);
        free(str);
        return nbytes;
    }
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(d);
    if (file == nullptr) {
        return SCE_KERNEL_ERROR_EBADF;
    }

    std::scoped_lock lk{file->m_mutex};
    return file->f.WriteRaw<u8>(buf, nbytes);
}

size_t PS4_SYSV_ABI _readv(int d, const SceKernelIovec* iov, int iovcnt) {
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(d);
    size_t total_read = 0;
    std::scoped_lock lk{file->m_mutex};
    for (int i = 0; i < iovcnt; i++) {
        total_read += file->f.ReadRaw<u8>(iov[i].iov_base, iov[i].iov_len);
    }
    return total_read;
}

s64 PS4_SYSV_ABI sceKernelLseek(int d, s64 offset, int whence) {
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(d);

    Common::FS::SeekOrigin origin{};
    if (whence == 0) {
        origin = Common::FS::SeekOrigin::SetOrigin;
    } else if (whence == 1) {
        origin = Common::FS::SeekOrigin::CurrentPosition;
    } else if (whence == 2) {
        origin = Common::FS::SeekOrigin::End;
    }

    std::scoped_lock lk{file->m_mutex};
    file->f.Seek(offset, origin);
    return file->f.Tell();
}

s64 PS4_SYSV_ABI posix_lseek(int d, s64 offset, int whence) {
    int result = sceKernelLseek(d, offset, whence);
    if (result < 0) {
        LOG_ERROR(Kernel_Pthread, "posix_lseek: error = {}", result);
        ErrSceToPosix(result);
        return -1;
    }
    return result;
}

s64 PS4_SYSV_ABI sceKernelRead(int d, void* buf, size_t nbytes) {
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(d);
    if (file == nullptr) {
        return SCE_KERNEL_ERROR_EBADF;
    }

    std::scoped_lock lk{file->m_mutex};
    return file->f.ReadRaw<u8>(buf, nbytes);
}

int PS4_SYSV_ABI posix_read(int d, void* buf, size_t nbytes) {
    int result = sceKernelRead(d, buf, nbytes);
    if (result < 0) {
        LOG_ERROR(Kernel_Pthread, "posix_read: error = {}", result);
        ErrSceToPosix(result);
        return -1;
    }
    return result;
}

int PS4_SYSV_ABI sceKernelMkdir(const char* path, u16 mode) {
    LOG_INFO(Kernel_Fs, "path = {} mode = {}", path, mode);
    if (path == nullptr) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    std::string dir_name = mnt->GetHostFile(path);
    if (std::filesystem::exists(dir_name)) {
        return SCE_KERNEL_ERROR_EEXIST;
    }

    // CUSA02456: path = /aotl after sceSaveDataMount(mode = 1)
    if (dir_name.empty() || !std::filesystem::create_directory(dir_name)) {
        return SCE_KERNEL_ERROR_EIO;
    }

    if (!std::filesystem::exists(dir_name)) {
        return SCE_KERNEL_ERROR_ENOENT;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI posix_mkdir(const char* path, u16 mode) {
    int result = sceKernelMkdir(path, mode);
    if (result < 0) {
        LOG_ERROR(Kernel_Pthread, "posix_mkdir: error = {}", result);
        ErrSceToPosix(result);
        return -1;
    }
    return result;
}

int PS4_SYSV_ABI sceKernelStat(const char* path, OrbisKernelStat* sb) {
    LOG_INFO(Kernel_Fs, "(PARTIAL) path = {}", path);
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const auto& path_name = mnt->GetHostFile(path);
    std::memset(sb, 0, sizeof(OrbisKernelStat));
    const bool is_dir = std::filesystem::is_directory(path_name);
    const bool is_file = std::filesystem::is_regular_file(path_name);
    if (!is_dir && !is_file) {
        return ORBIS_KERNEL_ERROR_ENOENT;
    }
    if (std::filesystem::is_directory(path_name)) {
        sb->st_mode = 0000777u | 0040000u;
        sb->st_size = 0;
        sb->st_blksize = 512;
        sb->st_blocks = 0;
        // TODO incomplete
    } else {
        sb->st_mode = 0000777u | 0100000u;
        sb->st_size = static_cast<int64_t>(std::filesystem::file_size(path_name));
        sb->st_blksize = 512;
        sb->st_blocks = (sb->st_size + 511) / 512;
        // TODO incomplete
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI posix_stat(const char* path, OrbisKernelStat* sb) {
    int result = sceKernelStat(path, sb);
    if (result < 0) {
        LOG_ERROR(Kernel_Pthread, "posix_stat: error = {}", result);
        ErrSceToPosix(result);
        return -1;
    }
    return result;
}

int PS4_SYSV_ABI sceKernelCheckReachability(const char* path) {
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    std::string path_name = mnt->GetHostFile(path);
    if (!std::filesystem::exists(path_name)) {
        return SCE_KERNEL_ERROR_ENOENT;
    }
    return ORBIS_OK;
}

s64 PS4_SYSV_ABI sceKernelPread(int d, void* buf, size_t nbytes, s64 offset) {
    if (d < 3) {
        return ORBIS_KERNEL_ERROR_EPERM;
    }
    if (offset < 0) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(d);
    if (file == nullptr) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    std::scoped_lock lk{file->m_mutex};
    const s64 pos = file->f.Tell();
    SCOPE_EXIT {
        file->f.Seek(pos);
    };
    file->f.Seek(offset);
    return file->f.ReadRaw<u8>(buf, nbytes);
}

int PS4_SYSV_ABI sceKernelFStat(int fd, OrbisKernelStat* sb) {
    LOG_INFO(Kernel_Fs, "(PARTIAL) fd = {}", fd);
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(fd);
    std::memset(sb, 0, sizeof(OrbisKernelStat));

    if (file->is_directory) {
        sb->st_mode = 0000777u | 0040000u;
        sb->st_size = 0;
        sb->st_blksize = 512;
        sb->st_blocks = 0;
        // TODO incomplete
    } else {
        sb->st_mode = 0000777u | 0100000u;
        sb->st_size = file->f.GetSize();
        sb->st_blksize = 512;
        sb->st_blocks = (sb->st_size + 511) / 512;
        // TODO incomplete
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI posix_fstat(int fd, OrbisKernelStat* sb) {
    int result = sceKernelFStat(fd, sb);
    if (result < 0) {
        LOG_ERROR(Kernel_Pthread, "posix_fstat: error = {}", result);
        ErrSceToPosix(result);
        return -1;
    }
    return result;
}

s32 PS4_SYSV_ABI sceKernelFsync(int fd) {
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(fd);
    file->f.Flush();
    return ORBIS_OK;
}

static int GetDents(int fd, char* buf, int nbytes, s64* basep) {
    // TODO error codes
    ASSERT(buf != nullptr);
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(fd);

    if (file->dirents_index == file->dirents.size()) {
        return ORBIS_OK;
    }

    const auto& entry = file->dirents.at(file->dirents_index++);
    auto str = entry.name;
    auto str_size = str.size() - 1;
    static int fileno = 1000; // random
    OrbisKernelDirent* sce_ent = (OrbisKernelDirent*)buf;
    sce_ent->d_fileno = fileno++; // TODO this should be unique but atm it changes maybe switch to a
                                  // hash or something?
    sce_ent->d_reclen = sizeof(OrbisKernelDirent);
    sce_ent->d_type = (entry.isFile ? 8 : 4);
    sce_ent->d_namlen = str_size;
    strncpy(sce_ent->d_name, str.c_str(), ORBIS_MAX_PATH);
    sce_ent->d_name[ORBIS_MAX_PATH] = '\0';

    if (basep != nullptr) {
        *basep = file->dirents_index;
    }

    return sizeof(OrbisKernelDirent);
}

int PS4_SYSV_ABI sceKernelGetdents(int fd, char* buf, int nbytes) {
    return GetDents(fd, buf, nbytes, nullptr);
}

int PS4_SYSV_ABI sceKernelGetdirentries(int fd, char* buf, int nbytes, s64* basep) {
    return GetDents(fd, buf, nbytes, basep);
}

s64 PS4_SYSV_ABI sceKernelPwrite(int d, void* buf, size_t nbytes, s64 offset) {
    if (d < 3) {
        return ORBIS_KERNEL_ERROR_EPERM;
    }
    if (offset < 0) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(d);
    if (file == nullptr) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    std::scoped_lock lk{file->m_mutex};
    const s64 pos = file->f.Tell();
    SCOPE_EXIT {
        file->f.Seek(pos);
    };
    file->f.Seek(offset);
    return file->f.WriteRaw<u8>(buf, nbytes);
}

void fileSystemSymbolsRegister(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("1G3lF1Gg1k8", "libkernel", 1, "libkernel", 1, 1, sceKernelOpen);
    LIB_FUNCTION("wuCroIGjt2g", "libScePosix", 1, "libkernel", 1, 1, posix_open);
    LIB_FUNCTION("UK2Tl2DWUns", "libkernel", 1, "libkernel", 1, 1, sceKernelClose);
    LIB_FUNCTION("bY-PO6JhzhQ", "libScePosix", 1, "libkernel", 1, 1, posix_close);
    LIB_FUNCTION("4wSze92BhLI", "libkernel", 1, "libkernel", 1, 1, sceKernelWrite);

    LIB_FUNCTION("+WRlkKjZvag", "libkernel", 1, "libkernel", 1, 1, _readv);
    LIB_FUNCTION("Oy6IpwgtYOk", "libkernel", 1, "libkernel", 1, 1, posix_lseek);
    LIB_FUNCTION("Oy6IpwgtYOk", "libScePosix", 1, "libkernel", 1, 1, posix_lseek);
    LIB_FUNCTION("oib76F-12fk", "libkernel", 1, "libkernel", 1, 1, sceKernelLseek);
    LIB_FUNCTION("Cg4srZ6TKbU", "libkernel", 1, "libkernel", 1, 1, sceKernelRead);
    LIB_FUNCTION("AqBioC2vF3I", "libScePosix", 1, "libkernel", 1, 1, posix_read);
    LIB_FUNCTION("1-LFLmRFxxM", "libkernel", 1, "libkernel", 1, 1, sceKernelMkdir);
    LIB_FUNCTION("JGMio+21L4c", "libScePosix", 1, "libkernel", 1, 1, posix_mkdir);
    LIB_FUNCTION("eV9wAD2riIA", "libkernel", 1, "libkernel", 1, 1, sceKernelStat);
    LIB_FUNCTION("kBwCPsYX-m4", "libkernel", 1, "libkernel", 1, 1, sceKernelFStat);
    LIB_FUNCTION("mqQMh1zPPT8", "libScePosix", 1, "libkernel", 1, 1, posix_fstat);

    LIB_FUNCTION("E6ao34wPw+U", "libScePosix", 1, "libkernel", 1, 1, posix_stat);
    LIB_FUNCTION("+r3rMFwItV4", "libkernel", 1, "libkernel", 1, 1, sceKernelPread);
    LIB_FUNCTION("uWyW3v98sU4", "libkernel", 1, "libkernel", 1, 1, sceKernelCheckReachability);
    LIB_FUNCTION("fTx66l5iWIA", "libkernel", 1, "libkernel", 1, 1, sceKernelFsync);
    LIB_FUNCTION("j2AIqSqJP0w", "libkernel", 1, "libkernel", 1, 1, sceKernelGetdents);
    LIB_FUNCTION("taRWhTJFTgE", "libkernel", 1, "libkernel", 1, 1, sceKernelGetdirentries);
    LIB_FUNCTION("nKWi-N2HBV4", "libkernel", 1, "libkernel", 1, 1, sceKernelPwrite);

    // openOrbis (to check if it is valid out of OpenOrbis
    LIB_FUNCTION("6c3rCVE-fTU", "libkernel", 1, "libkernel", 1, 1,
                 posix_open); // _open shoudld be equal to open function
}

} // namespace Libraries::Kernel
