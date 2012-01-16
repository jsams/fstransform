/*
 * io/io_posix.cc
 *
 *  Created on: Feb 28, 2011
 *      Author: max
 */

#include "../first.hh"

#include <cerrno>         // for errno
#include <cstdlib>        // for malloc(), free(), posix_fallocate()
#include <cstring>        // for memset()
#include <sys/types.h>    // for open()
#include <sys/stat.h>     //  "    "
#include <fcntl.h>        //  "    "
#include <unistd.h>       // for close(), unlink()
#include <sys/mman.h>     // for mmap(), munmap()

#include "../log.hh"      // for ff_log()
#include "../util.hh"     // for ff_max2(), ff_min2()

#include "../ui/ui.hh"    // for fr_ui

#include "extent_posix.hh" // for ft_extent_posixs()
#include "util_posix.hh"   // for ft_posix_*() misc functions
#include "io_posix.hh"     // for fr_io_posix


FT_IO_NAMESPACE_BEGIN

#if defined(MAP_ANONYMOUS)
#  define FC_MAP_ANONYMOUS MAP_ANONYMOUS
#elif defined(MAP_ANON)
#  define FC_MAP_ANONYMOUS MAP_ANON
#else
#  error both MAP_ANONYMOUS and MAP_ANON are missing, cannot compile io_posix.cc
#endif


/** default constructor */
fr_io_posix::fr_io_posix(fr_job & job)
: super_type(job), storage_mmap(MAP_FAILED), buffer_mmap(MAP_FAILED),
  storage_mmap_size(0), buffer_mmap_size(0)
{
    /* mark fd[] as invalid: they are not open yet */
    for (ft_size i = 0; i < FC_ALL_FILE_COUNT; i++)
        fd[i] = -1;

    /* tell superclass that we will invoke ui methods by ourselves */
    delegate_ui(true);
}

/** destructor. calls close() */
fr_io_posix::~fr_io_posix()
{
    close();
}

/** return true if a single descriptor/stream is open */
bool fr_io_posix::is_open0(ft_size i) const
{
    return fd[i] >= 0;
}

/** close a single descriptor/stream */
void fr_io_posix::close0(ft_size i)
{
    if (i < FC_ALL_FILE_COUNT && fd[i] >= 0) {
        if (::close(fd[i]) != 0)
            ff_log(FC_WARN, errno, "closing %s file descriptor [%d] failed", label[i], fd[i]);
        fd[i] = -1;
    }
}

/** return true if this fr_io_posix is currently (and correctly) open */
bool fr_io_posix::is_open() const
{
    return dev_length() != 0 && is_open0(FC_DEVICE);
}

/** check for consistency and open DEVICE, LOOP-FILE and ZERO-FILE */
int fr_io_posix::open(const fr_args & args)
{
    if (is_open()) {
        // already open!
        ff_log(FC_ERROR, 0, "unexpected call, I/O is already open");
        return EISCONN;
    }
    int err = super_type::open(args);
    if (err != 0)
        return err;

    if (getuid() != 0) {
        ff_log(FC_WARN, 0, "not running as root! expect '%s' errors", strerror(EPERM));
    }

    char const* const* path = args.io_args;
    ft_uoff len[FC_FILE_COUNT];
    ft_size i;
    ft_dev dev[FC_FILE_COUNT];
    const bool force = force_run();
    const char * force_msg = force ? ", continuing due to '-f'" : ", use '-f' to override";

    for (i = 0; i < FC_FILE_COUNT; i++)
        dev[i] = 0;

    do {
        for (i = 0; i < FC_FILE_COUNT; i++) {
            if ((fd[i] = ::open(path[i], i == FC_DEVICE ? O_RDWR : O_RDONLY)) < 0) {
                err = ff_log(FC_ERROR, errno, "error opening %s '%s'", label[i], path[i]);
                break;
            }

            if (i == FC_DEVICE)
                /* for DEVICE, we want to know its dev_t */
                err = ff_posix_blkdev_dev(fd[i], & dev[i]);
            else
                /* for LOOP-FILE and ZERO-FILE, we want to know the dev_t of the device they are stored into */
                err = ff_posix_dev(fd[i], & dev[i]);

            if (err != 0) {
                err = ff_log((force ? FC_WARN : FC_ERROR), err, "%sfailed %s fstat('%s')%s",
                             (force ? "WARNING: " : ""), label[i], path[i], force_msg);
                if (force)
                    err = 0;
                else
                    break;
            }

            if (i == FC_DEVICE) {
                /* for DEVICE, we also want to know its length */
                if ((err = ff_posix_blkdev_size(fd[i], & len[i])) != 0) {
                    err = ff_log(FC_ERROR, err, "error in %s ioctl('%s', BLKGETSIZE64)", label[i], path[i]);
                    break;
                }
                /* device length is retrieved ONLY here. we must remember it */
                dev_length(len[i]);
                /* also remember device path */
                dev_path(path[i]);

                double pretty_len;
                const char * pretty_label = ff_pretty_size(len[i], & pretty_len);
                ff_log(FC_INFO, 0, "%s length is %.2f %sbytes", label[i], pretty_len, pretty_label);

            } else {
                /* for LOOP-FILE and ZERO-FILE, we check their length */
                if ((err = ff_posix_size(fd[i], & len[i])) != 0) {
                    err = ff_log((force ? FC_WARN : FC_ERROR), err, "%sfailed %s fstat('%s')%s",
                                 (force ? "WARNING: " : ""), label[i], path[i], force_msg);
                    if (force)
                        err = 0;
                    else
                        break;
                } else if (len[i] > len[FC_DEVICE]) {
                    err = ff_log(FC_ERROR, 0, "%s size = %"FT_ULL" bytes exceeds %s length = %"FT_ULL" bytes",
                                 label[i], (ft_ull)len[i], label[FC_DEVICE], (ft_ull)len[FC_DEVICE]);
                    break;
                } else if (i == FC_LOOP_FILE && len[i] < len[FC_DEVICE]) {
                    ff_log(FC_INFO, 0, "%s '%s' is shorter than %s, remapping will also shrink file-system",
                                 label[i], path[i], label[FC_DEVICE]);
                }
                /* remember LOOP-FILE length */
                if (i == FC_LOOP_FILE)
                	loop_file_length(len[i]);

                /* for LOOP-FILE and ZERO-FILE, we also check that they are actually contained in DEVICE */
                if (dev[FC_DEVICE] != dev[i]) {
                    ff_log((force ? FC_WARN : FC_ERROR), 0, "%s'%s' is device 0x%04x, but %s '%s' is contained in device 0x%04x%s",
                        (force ? "WARNING: " : ""), path[FC_DEVICE], (unsigned)dev[FC_DEVICE], label[i], path[i], (unsigned)dev[i], force_msg);
                    if (force)
                        break;
                    else
                        err = -EINVAL;
                }
            }
        }
    } while (0);

    if (err != 0)
        close();

    return err;
}



/**
 * close file descriptors.
 * return 0 for success, 1 for error (logs by itself error message)
 */
void fr_io_posix::close()
{
    close_storage();
    for (ft_size i = 0; i < FC_FILE_COUNT; i++)
        close0(i);

    super_type::close();
}



/** return true if this I/O has open descriptors/streams to LOOP-FILE and FREE-SPACE */
bool fr_io_posix::is_open_extents() const
{
    bool flag = false;
    if (dev_length() != 0) {
        ft_size which[] = { FC_LOOP_FILE, FC_ZERO_FILE };
        ft_size i, n = sizeof(which)/sizeof(which[0]);
        for (i = 0; i < n; i++)
            if (!is_open0(which[i]) < 0)
                break;
        flag = i == n;
    }
    return flag;
}

/**
 * retrieve LOOP-FILE extents and FREE-SPACE extents and insert them into
 * the vectors loop_file_extents and free_space_extents.
 * the vectors will be ordered by extent ->logical.
 *
 * return 0 for success, else error (and vectors contents will be UNDEFINED).
 *
 * if success, also returns in ret_effective_block_size_log2 the log2()
 * of device effective block size.
 * the device effective block size is defined as follows:
 * it is the largest power of 2 that exactly divides all physical,
 * logical and lengths in all returned extents (both for LOOP-FILE
 * and for FREE-SPACE) and that also exactly exactly divides device length.
 *
 * must be overridden by sub-classes.
 *
 * a common trick subclasses may use to implement this method
 * is to fill the device's free space with a ZERO-FILE,
 * and actually retrieve the extents used by ZERO-FILE.
 */
int fr_io_posix::read_extents(fr_vector<ft_uoff> & loop_file_extents,
                              fr_vector<ft_uoff> & free_space_extents,
                              ft_uoff & ret_block_size_bitmask)
{
    ft_uoff block_size_bitmask = ret_block_size_bitmask;
    int err = 0;
    do {
        if (!is_open_extents()) {
            err = ENOTCONN; // not open!
            break;
        }

        /* ff_read_extents_posix() appends into fr_vector<T>, does NOT overwrite it */
        if ((err = ff_read_extents_posix(fd[FC_LOOP_FILE], dev_length(), loop_file_extents, block_size_bitmask)) != 0)
            break;
        if ((err = ff_read_extents_posix(fd[FC_ZERO_FILE], dev_length(), free_space_extents, block_size_bitmask)) != 0)
            break;

    } while (0);

    if (err == 0)
        ret_block_size_bitmask = block_size_bitmask;

    return err;
}


/**
 * close the file descriptors for LOOP-FILE and ZERO-FILE
 */
void fr_io_posix::close_extents()
{
    ft_size which[] = { FC_LOOP_FILE, FC_ZERO_FILE };
    for (ft_size i = 0; i < sizeof(which)/sizeof(which[0]); i++)
        close0(which[i]);
}

/** close and munmap() SECONDARY-STORAGE. called by close() and by work<T>::close_storage() */
int fr_io_posix::close_storage()
{
    int err = 0;
    enum { i = FC_PRIMARY_STORAGE, j = FC_SECONDARY_STORAGE };
    if (storage_mmap != MAP_FAILED) {
        if (munmap(storage_mmap, storage_mmap_size) == 0) {
            storage_mmap = MAP_FAILED;
            storage_mmap_size = 0;
        } else {
            bool flag_i = !primary_storage().empty();
            bool flag_j = secondary_storage().length() != 0;
            err = ff_log(FC_WARN, errno, "warning: %s%s%s munmap() failed",
                         (flag_i ? label[i] : ""),
                         (flag_i && flag_j ? " and " : ""),
                         (flag_j ? label[j] : "")
            );
        }
    }
    if (err == 0 && buffer_mmap != MAP_FAILED) {
        if (munmap(buffer_mmap, buffer_mmap_size) == 0) {
            buffer_mmap = MAP_FAILED;
            buffer_mmap_size = 0;
        } else {
            err = ff_log(FC_WARN, errno, "warning: memory buffer munmap() failed");
        }
    }
    if (err == 0) {
        close0(i);
        close0(j);
    }
    return err;
}

/**
 * create and open SECONDARY-STORAGE job.job_dir() + '.storage',
 * fill it with 'secondary_len' bytes of zeros and mmap() it.
 *
 * then mmap() this->primary_storage extents.
 * finally setup a virtual storage composed by 'primary_storage' extents inside DEVICE, plus secondary-storage extents.
 *
 * return 0 if success, else error
 */
int fr_io_posix::create_storage(ft_size secondary_size, ft_size mem_buffer_size)
{
    /*
     * how to get a contiguous mmapped() memory for all the extents in primary_storage and secondary_storage:
     *
     * mmap(MAP_ANON/MAP_ANONYMOUS) the total storage size (sum(primary_storage->length)+secondary_len)
     * then incrementally replace parts of it with munmap() followed by mmap(MAP_FIXED) of each storage extent
     */
    enum { i = FC_PRIMARY_STORAGE, j = FC_SECONDARY_STORAGE };

    if (storage_mmap != MAP_FAILED || is_open0(j)) {
        // already initialized!
        ff_log(FC_ERROR, 0, "unexpected call to create_storage(), %s is already initialized",
               storage_mmap != MAP_FAILED ? label[i] : label[j]);
        // return error as already reported
        return -EISCONN;
    }

    /**
     * recompute primary_len... we could receive it from caller, but it's redundant
     * and in any case we will still need to iterate on primary_storage to mmap() it
     */
    ft_uoff primary_len = 0;
    fr_vector<ft_uoff>::iterator begin = primary_storage().begin(), iter, end = primary_storage().end();
    for (iter = begin; iter != end; ++iter) {
        primary_len += iter->second.length;
    }

    double pretty_len;
    const char * pretty_label;
    int err = 0;
    do {
        const ft_size mmap_size = (ft_size) primary_len + secondary_size;
        if (primary_len > (ft_size)-1 - secondary_size) {
            err = ff_log(FC_FATAL, EOVERFLOW, "internal error, %s + %s total length = %"FT_ULL" is larger than addressable memory",
                         label[i], label[j], (ft_ull) primary_len + secondary_size);
            break;
        }
        /*
         * mmap() total length as PROT_NONE, FC_MAP_ANONYMOUS.
         * used to reserve a large enough contiguous memory area
         * to mmap() PRIMARY STORAGE and SECONDARY STORAGE
         */
        storage_mmap = mmap(NULL, mmap_size, PROT_NONE, MAP_PRIVATE|FC_MAP_ANONYMOUS, -1, 0);
        if (storage_mmap == MAP_FAILED) {
            err = ff_log(FC_ERROR, errno, "%s: error preemptively reserving contiguous RAM: mmap(length = %"FT_ULL", PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1) failed",
                    label[FC_STORAGE], (ft_ull) mmap_size);
            break;
        } else
            ff_log(FC_DEBUG, 0, "%s: preemptively reserved contiguous RAM,"
                    " mmap(length = %"FT_ULL", PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1) = ok",
                    label[FC_STORAGE], (ft_ull) mmap_size);
        storage_mmap_size = mmap_size;
        /*
         * mmap() another area, mem_buffer_size bytes long, as PROT_READ|PROT_WRITE, FC_MAP_ANONYMOUS.
         * used as memory buffer during DEV2DEV copies
         */
        buffer_mmap = mmap(NULL, mem_buffer_size, PROT_READ|PROT_WRITE, MAP_PRIVATE|FC_MAP_ANONYMOUS, -1, 0);
        if (buffer_mmap == MAP_FAILED) {
            err = ff_log(FC_ERROR, errno, "%s: error allocating memory buffer: mmap(length = %"FT_ULL", PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1) failed",
                    label[FC_STORAGE], (ft_ull) mem_buffer_size);
            break;
        }
        /*
         * we could mlock(buffer_mmap), but it's probably excessive
         * as it constraints too much the kernel in deciding the memory to swap to disk.
         *
         * instead, we opt for a simple memset(), which forces the kernel to actually allocate
         * the RAM for us (we do not want memory overcommit errors later on),
         * but still let the kernel decide what to swap to disk
         */
        memset(buffer_mmap, '\0', buffer_mmap_size = mem_buffer_size);

        pretty_len = 0.0;
        pretty_label = ff_pretty_size(buffer_mmap_size, & pretty_len);

        ff_log(FC_NOTICE, 0, "allocated %.2f %sbytes RAM as memory buffer", pretty_len, pretty_label);




        if (secondary_size != 0) {
            if ((err = create_secondary_storage(secondary_size)) != 0)
                break;
        } else
            ff_log(FC_INFO, 0, "not creating %s, %s is large enough", label[j], label[i]);

        /* now incrementally replace storage_mmap with actually mmapped() storage extents */
        begin = primary_storage().begin();
        end = primary_storage().end();
        ft_size mem_offset = 0;
        for (iter = begin; err == 0 && iter != end; ++iter)
            err = replace_storage_mmap(fd[FC_DEVICE], label[i], *iter, iter-begin, mem_offset);
        if (err != 0)
            break;

        if (secondary_size != 0) {
            if ((err = replace_storage_mmap(fd[j], label[j], secondary_storage(), 0, mem_offset)) != 0)
                break;
        }
        if (mem_offset != storage_mmap_size) {
            ff_log(FC_FATAL, 0, "internal error, mapped %s extents in RAM used %"FT_ULL" bytes instead of expected %"FT_ULL" bytes",
                    label[FC_STORAGE], (ft_ull) mem_offset, (ft_ull) storage_mmap_size);
            err = EINVAL;
        }
    } while (0);

    if (err == 0) {
        pretty_len = 0.0;
        pretty_label = ff_pretty_size(storage_mmap_size, & pretty_len);

        ff_log(FC_NOTICE, 0, "%s%s%s is %.2f %sbytes, initialized and mmapped() to contiguous RAM",
                (primary_len != 0 ? label[i] : ""),
                (primary_len != 0 && secondary_size != 0 ? " + " : ""),
                (secondary_size != 0 ? label[j] : ""),
                pretty_len, pretty_label);
    } else
        close_storage();

    return err;
}

/**
 * replace a part of the mmapped() storage_mmap area with specified storage_extent,
 * and store mmapped() address into storage_extent.user_data().
 * return 0 if success, else error
 *
 * note: fd shoud be this->fd[FC_DEVICE] for primary storage,
 * or this->fd[FC_SECONDARY_STORAGE] for secondary storage
 */
int fr_io_posix::replace_storage_mmap(int fd, const char * label_i,
        fr_extent<ft_uoff> & storage_extent, ft_size extent_index,
        ft_size & ret_mem_offset)
{
    ft_size len = (ft_size) storage_extent.length();
    ft_size mem_start = ret_mem_offset, mem_end = mem_start + len;
    int err = 0;
    do {
        if (mem_start >= storage_mmap_size || mem_end > storage_mmap_size) {
            ff_log(FC_FATAL, 0, "internal error mapping %s extent #%"FT_ULL" in RAM!"
                    " extent (%"FT_ULL", length = %"FT_ULL") overflows total %s length = %"FT_ULL,
                    label_i, (ft_ull) extent_index, (ft_ull) mem_start, (ft_ull) len,
                    label[FC_STORAGE], (ft_ull) storage_mmap_size);
            /* mark error as reported */
            err = -EINVAL;
            break;
        }
        void * addr_old = (char *) storage_mmap + mem_start;
        if (munmap(addr_old, len) != 0) {
            err = ff_log(FC_ERROR, errno, "error mapping %s extent #%"FT_ULL" in RAM,"
                    " munmap(address + %"FT_ULL", length = %"FT_ULL") failed",
                    label_i, (ft_ull) extent_index, (ft_ull) mem_start, (ft_ull) len);
            break;
        }
        void * addr_new = mmap(addr_old, len, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_FIXED, fd, storage_extent.physical());
        if (addr_new == MAP_FAILED) {
            err = ff_log(FC_ERROR, errno, "error mapping %s extent #%"FT_ULL" in RAM,"
                    " mmap(address + %"FT_ULL", length = %"FT_ULL", MAP_FIXED) failed",
                    label_i, (ft_ull) extent_index, (ft_ull) mem_start, (ft_ull) len);
            break;
        }
        if (addr_new != addr_old) {
            ff_log(FC_ERROR, 0, "error mapping %s extent #%"FT_ULL" in RAM,"
                    " mmap(address + %"FT_ULL", length = %"FT_ULL", MAP_FIXED)"
                   " violated MAP_FIXED and returned a different address",
                   label_i, (ft_ull) extent_index, (ft_ull) mem_start, (ft_ull) len);
            /* mark error as reported */
            err = -EFAULT;
            /** try at least to munmap() this problematic extent */
            if (munmap(addr_new, len) != 0) {
                ff_log(FC_WARN, errno, "weird OS! not only mmap() violated MAP_FIXED, but subsequent munmap() failed too");
            }
            break;
        }
        ff_log(FC_TRACE, 0, "%s extent #%"FT_ULL" mapped in RAM,"
                " mmap(address + %"FT_ULL", length = %"FT_ULL", MAP_FIXED) = ok",
                label_i, (ft_ull) extent_index, (ft_ull) mem_start, (ft_ull) len);

        if (!simulate_run() && mlock(addr_new, len) != 0) {
            ff_log(FC_WARN, errno, "warning: %s extent #%"FT_ULL" mlock(address + %"FT_ULL", length = %"FT_ULL") failed",
                   label_i, (ft_ull) extent_index, (ft_ull) mem_start, (ft_ull) len);
        }
        /**
         * all ok, let's store mmapped() address offset into extent.user_data to remember it,
         * as msync() inside flush() and munmap() inside close_storage() could need it
         */
        storage_extent.user_data() = mem_start;
        /* and remember to update ret_mem_offset */
        ret_mem_offset += len;

    } while (0);
    return err;
}


/**
 * create and open SECONDARY-STORAGE in job.job_dir() + '.storage'
 * and fill it with 'secondary_len' bytes of zeros. do not mmap() it.
 * return 0 if success, else error
 */
int fr_io_posix::create_secondary_storage(ft_size len)
{
    enum { j = FC_SECONDARY_STORAGE };

    ft_string filepath = job_dir();
    filepath += "/storage.bin";
    const char * path = filepath.c_str();
    int err = 0;

    do {
        const ft_off s_len = (ft_off) len;
        if (s_len < 0 || len != (ft_size) s_len) {
            err = ff_log(FC_FATAL, EOVERFLOW, "internal error, %s length = %"FT_ULL" overflows type (off_t)", label[j], (ft_ull) len);
            break;
        }
        
        if ((fd[j] = ::open(path, O_RDWR|O_CREAT|O_TRUNC, 0600)) < 0) {
            err = ff_log(FC_ERROR, errno, "error in %s open('%s')", label[j], path);
            break;
        }

        double pretty_len = 0.0;
        const char * pretty_label = ff_pretty_size(len, & pretty_len);
        const bool simulated = simulate_run();

        ff_log(FC_INFO, 0, "%s:%s writing %.2f %sbytes to '%s' ...", label[j], (simulated ? " (simulated)" : ""), pretty_len, pretty_label, path);

        if (simulated) {
            if ((err = ff_posix_lseek(fd[j], len - 1)) != 0) {
                err = ff_log(FC_ERROR, errno, "error in %s lseek('%s', offset = %"FT_ULL" - 1)", label[j], path, (ft_ull)len);
                break;
            }
            char zero = '\0';
            if ((err = ff_posix_write(fd[j], & zero, 1)) != 0) {
                err = ff_log(FC_ERROR, errno, "error in %s write('%s', '\\0', length = 1)", label[j], path);
                break;
            }
        } else {
            
#ifdef FT_HAVE_POSIX_FALLOCATE
            /* try with posix_fallocate() */
            if ((err = posix_fallocate(fd[j], 0, s_len)) != 0) {
#endif /* FT_HAVE_POSIX_FALLOCATE */
                
                /* else fall back on write() */
                enum { zero_len = 64*1024 };
                char zero[zero_len];
                ft_size pos = 0, chunk;
                
                while (pos < len) {
                    chunk = ff_min2<ft_size>(zero_len, len - pos);
                    if ((err = ff_posix_write(fd[j], zero, chunk)) != 0) {
                        err = ff_log(FC_ERROR, errno, "error in %s write('%s')", label[j], path);
                        break;
                    }
                    pos += chunk;
                }
#ifdef FT_HAVE_POSIX_FALLOCATE
            }
#endif /* FT_HAVE_POSIX_FALLOCATE */
        }
        
        /* remember secondary_storage details */
        fr_extent<ft_uoff> & extent = secondary_storage();
        extent.physical() = extent.logical() = 0;
        extent.length() = len;

        ff_log(FC_INFO, 0, "%s:%s file created", label[j], (simulated ? " (simulated)" : ""));

    } while (0);

    if (err != 0) {
        const bool need_unlink = is_open0(j);
        close0(fd[j]);
        if (need_unlink && unlink(path) != 0)
            ff_log(FC_WARN, errno, "removing %s file '%s' failed", label[j], path);
    }
    return err;
}

/** call umount(8) on dev_path() */
int fr_io_posix::umount_dev()
{
    const char * cmd = umount_cmd(), * dev = dev_path();
    const char * arg0 = NULL;
    char * tofree = NULL;

    std::vector<const char *> args;

    if (cmd == NULL) {
        // posix standard name for umount(8)
        args.push_back(cmd = "/bin/umount");
        // only one argument: device path
        args.push_back(arg0 = dev);
    } else {
        tofree = strdup(cmd);
        char * wcmd = tofree;
        if (wcmd == NULL)
            return ff_log(FC_ERROR, ENOMEM, "out of memory! failed to strdup('%s')", cmd);

        char * space;
        /*
         * split --umount-cmd CMD, assuming arguments are delimited by spaces.
         * prevents passing files, directories or options containing space...
         * in such case please create a script and give its path with --umount-cmd CMD
         */
        for (;;) {
            while (*wcmd == ' ')
                wcmd++;
            if (*wcmd == '\0')
                break;

            args.push_back(wcmd);
            if ((space = strchr(wcmd, ' ')) == NULL)
                break;
            * space = '\0';
            wcmd = space + 1;
        }
    }
    args.push_back(NULL); // needed by ff_posix_exec() as end-of-arguments marker

    ff_log(FC_INFO, 0, "unmounting %s '%s'... command: %s%s%s", label[FC_DEVICE],
           dev, cmd, arg0 != NULL ? " " : "", arg0 != NULL ? arg0 : "");

    int err = ff_posix_exec(args[0], & args[0]);

    if (err == 0)
        ff_log(FC_NOTICE, 0, "successfully unmounted %s '%s'", label[FC_DEVICE], dev);

    if (tofree != NULL)
        ::free(tofree);
    return err;
}


/**
 * actually copy a list of fragments from DEVICE to STORAGE, or from STORAGE or DEVICE, or from DEVICE to DEVICE.
 * note: parameters are in bytes!
 * return 0 if success, else error.
 *
 * we expect request_vec to be sorted by ->physical (i.e. ->from_physical)
 */
int fr_io_posix::flush_copy_bytes(fr_dir dir, fr_vector<ft_uoff> & request_vec)
{
    int err = 0;


    switch (dir) {
    case FC_DEV2STORAGE: {
        /* from DEVICE to memory-mapped STORAGE */
        /* sequential disk access: request_vec is supposed to be already sorted by device from_offset, i.e. extent->physical */
        fr_vector<ft_uoff>::const_iterator iter = request_vec.begin(), end = request_vec.end();
        for (; err == 0 && iter != end; ++iter)
            err = flush_copy_bytes(FC_POSIX_DEV2STORAGE, *iter);
        break;
    }
    case FC_STORAGE2DEV: {
        /* from memory-mapped STORAGE to DEVICE */
        /* sequential disk access: request_vec is supposed to be already sorted by device to_offset, i.e. extent->logical */
        fr_vector<ft_uoff>::const_iterator iter = request_vec.begin(), end = request_vec.end();
        for (; err == 0 && iter != end; ++iter)
            err = flush_copy_bytes(FC_POSIX_STORAGE2DEV, *iter);
        break;
    }
    case FC_DEV2DEV: {
        /* from DEVICE to DEVICE, using RAM buffer */
        /* sequential disk access: request_vec is supposed to be sorted by device to_offset, i.e. extent->logical */

        request_vec.sort_by_physical(); /* sort by device from_offset, i.e. extent->physical */

        ft_uoff from_offset, to_offset, length;
        ft_size buf_offset = 0, buf_free = buffer_mmap_size, buf_length;

        ft_size start = 0, i = start, save_i, n = request_vec.size();

        do {
            /* iterate and fill buffer_mmap */
            for (; err == 0 && buf_free != 0 && i < n; ++i) {
                fr_extent<ft_uoff> & extent = request_vec[i];
                if ((length = extent.length()) > (ft_uoff) buf_free)
                    break;
                if ((err = flush_copy_bytes(FC_POSIX_DEV2RAM, extent.physical(), (ft_uoff)(extent.user_data() = buf_offset), length)) != 0)
                    break;
                buf_offset += (ft_size) length;
                buf_free -= (ft_size) length;
            }
            if (err != 0)
                break;

            /* buffer_mmap is now (almost) full. sort buffered data by device to_offset (i.e. extent->logical) and write it to target */
            if ((save_i = i) != start) {
                request_vec.sort_by_logical(request_vec.begin() + start, request_vec.begin() + i);

                for (i = start; err == 0 && i != save_i; ++i) {
                    fr_extent<ft_uoff> & extent = request_vec[i];
                    if ((err = flush_copy_bytes(FC_POSIX_RAM2DEV, (ft_uoff) extent.user_data(), extent.logical(), extent.length())) != 0)
                        break;
                }
                if (err != 0)
                    break;
            }

            if (err != 0 || (err = flush_bytes()) != 0)
                break;

            /* buffered data written to target. now there may be one or more extents NOT fitting into buffer_mmap */
            buf_offset = 0, buf_free = buffer_mmap_size;
            for (i = save_i; err == 0 && i != n; ++i) {
                fr_extent<ft_uoff> & extent = request_vec[i];
                if ((length = extent.length()) <= buf_free)
                    break;

                from_offset = extent.physical();
                to_offset = extent.logical();
                while (length != 0) {
                    buf_length = (ft_size) ff_min2<ft_uoff>(length, buf_free);

                    if ((err = flush_copy_bytes(FC_POSIX_DEV2RAM, from_offset, buf_offset, buf_length)) != 0
                        || (err = flush_copy_bytes(FC_POSIX_RAM2DEV, buf_offset, to_offset, buf_length)) != 0
                        || (err = flush_bytes()) != 0)
                        break;

                    length -= (ft_uoff) buf_length;
                }
                if (err != 0)
                    break;
            }
        } while (err == 0 && (start = i) != n);
        break;
    }
    default:
        /* from STORAGE to STORAGE */
        err = ff_log(FC_FATAL, ENOSYS, "internal error! unexpected call to io_posix.copy_bytes(), STORAGE to STORAGE copies are not supposed to be used");
        break;
    }
    return err;
}


int fr_io_posix::flush_copy_bytes(fr_dir_posix dir, const fr_extent<ft_uoff> & request)
{
    return flush_copy_bytes(dir, request.physical(), request.logical(), request.length());
}

#undef ENABLE_CHECK_IF_MEM_IS_ZERO

#ifdef ENABLE_CHECK_IF_MEM_IS_ZERO
/** returns true if the specified memory range contains ONLY zeroes. */
static bool fr_io_posix_mem_is_zero(const char * mem_address, ft_size mem_length) {
    for (ft_size i = 0; i < mem_length; i++) {
        if (mem_address[i] != 0)
            return false;
    }
    return true;
}
#endif // ENABLE_CHECK_IF_MEM_IS_ZERO



int fr_io_posix::flush_copy_bytes(fr_dir_posix dir, ft_uoff from_offset, ft_uoff to_offset, ft_uoff length)
{
    const bool use_storage = dir == FC_POSIX_DEV2STORAGE || dir == FC_POSIX_STORAGE2DEV;
    const bool read_dev = dir == FC_POSIX_DEV2STORAGE || dir == FC_POSIX_DEV2RAM;

    const char * label_dev   = label[FC_DEVICE];
    const char * label_other = use_storage ? label[FC_STORAGE] : "RAM";
    const char * label_from = read_dev ? label_dev : label_other;
    const char * label_to = read_dev ? label_other : label_dev;

    const ft_size mmap_size = use_storage ? storage_mmap_size : buffer_mmap_size;

    const ft_uoff dev_offset = read_dev ? from_offset : to_offset;
    const ft_uoff other_offset = read_dev ? to_offset : from_offset;

    /* validate("label", N, ...) also checks if from/to + length overflows (ft_size)-1 */
    int err = validate("ft_size", (ft_uoff)(ft_size)-1, dir, from_offset, to_offset, length);
    if (err == 0)
        err = validate("ft_size", (ft_uoff)mmap_size, dir, 0, other_offset, length);
    if (err != 0)
        return err;

    const ft_size mem_offset = (ft_size)other_offset;
    const ft_size mem_length = (ft_size)length;

    char * mmap_address = use_storage ? (char *)storage_mmap : (char *)buffer_mmap;
    const int fd = this->fd[FC_DEVICE];
    const bool simulated = simulate_run();

    if (ui() != NULL) {
        if (dir != FC_POSIX_RAM2DEV) {
            fr_from from = dir == FC_POSIX_STORAGE2DEV ? FC_FROM_STORAGE : FC_FROM_DEV;
            ui()->show_io_read(from, from_offset, length);
        }
        if (dir!= FC_POSIX_DEV2RAM) {
            fr_to to = dir == FC_POSIX_DEV2STORAGE ? FC_TO_STORAGE : FC_TO_DEV;
            ui()->show_io_write(to, to_offset, length);
        }
    }
    do {
        if (!simulated) {
            if ((err = ff_posix_lseek(fd, dev_offset)) != 0) {
                err = ff_log(FC_ERROR, err, "I/O error in %s lseek(fd = %d, offset = %"FT_ULL", SEEK_SET)",
                        label_dev, fd, (ft_ull)dev_offset);
                break;
            }
#define CURRENT_OP_FMT "from %s to %s, %s({fd = %d, offset = %"FT_ULL"}, address + %"FT_ULL", length = %"FT_ULL")"
#define CURRENT_OP_ARGS label_from, label_to, (read_dev ? "read" : "write"), fd, (ft_ull)dev_offset, (ft_ull)mem_offset, (ft_ull)mem_length

#ifdef ENABLE_CHECK_IF_MEM_IS_ZERO
# define CHECK_IF_MEM_IS_ZERO \
            do { \
                if (fr_io_posix_mem_is_zero(mmap_address + mem_offset, mem_length)) { \
                    ff_log(FC_WARN, 0, "found an extent full of zeros copying " CURRENT_OP_FMT ". Stopping, press ENTER to continue.", CURRENT_OP_ARGS); \
                    char ch; \
                    (void) ff_posix_read(0, &ch, 1); \
                } \
            } while (0)

#else // !ENABLE_CHECK_IF_MEM_IS_ZERO

# define CHECK_IF_MEM_IS_ZERO do { } while (0)

#endif // ENABLE_CHECK_IF_MEM_IS_ZERO
            
            if (read_dev) {
                err = ff_posix_read(fd, mmap_address + mem_offset, mem_length);
                if (err == 0) {
                    CHECK_IF_MEM_IS_ZERO;
                }
            } else {
                CHECK_IF_MEM_IS_ZERO;
                err = ff_posix_write(fd, mmap_address + mem_offset, mem_length);
            }
            if (err != 0) {
                err = ff_log(FC_ERROR, err, "I/O error while copying " CURRENT_OP_FMT, CURRENT_OP_ARGS);
                break;
            }
        }
        ff_log(FC_TRACE, 0, "%scopy " CURRENT_OP_FMT " = ok",
               (simulated ? "(simulated) " : ""), CURRENT_OP_ARGS);
    } while (0);
    return err;
}


/* return (-)EOVERFLOW if request from/to + length overflow specified maximum value */
int fr_io_posix::validate(const char * type_name, ft_uoff type_max, fr_dir_posix dir2, ft_uoff from, ft_uoff to, ft_uoff length)
{
    fr_dir dir;
    switch (dir2) {
        case FC_POSIX_STORAGE2DEV: dir = FC_STORAGE2DEV; break;
        case FC_POSIX_DEV2STORAGE: dir = FC_DEV2STORAGE; break;
        case FC_POSIX_DEV2RAM:
        case FC_POSIX_RAM2DEV:     dir = FC_DEV2DEV;     break;
        default:                   dir = FC_INVALID2INVALID; break;
    }
    return super_type::validate(type_name, type_max, dir, from, to, length);
}


/**
 * flush any I/O specific buffer
 * return 0 if success, else error
 * implementation: call msync() because we use a mmapped() buffer for STORAGE,
 * and call sync() because we write() to DEVICE
 */
int fr_io_posix::flush_bytes()
{
    int err = 0;
    do {
        if (ui() != NULL)
            ui()->show_io_flush();

        if (simulate_run())
            break;

#if 1
        if ((err = msync(storage_mmap, storage_mmap_size, MS_SYNC)) != 0) {
            ff_log(FC_WARN, errno, "I/O error in %s msync(address + %"FT_ULL", length = %"FT_ULL")",
                    label[FC_STORAGE], (ft_ull)0, (ft_ull)storage_mmap_size);
            err = 0;
        }
#else
        fr_vector<ft_uoff>::const_iterator begin = primary_storage().begin(), iter, end = primary_storage().end();
        for (iter = begin; iter != end; ++iter)
            msync_bytes(*iter);
        if (secondary_storage().length() != 0)
            msync_bytes(secondary_storage());
#endif

        (void) sync(); // sync() returns void
#if 0
        if (err != 0) {
            ff_log(FC_WARN, errno, "I/O error in sync()");
            err = 0;
        }
#endif
    } while (0);
    return err;
}

/** internal method, called by flush_bytes() to perform msync() on mmapped storage */
int fr_io_posix::msync_bytes(const fr_extent<ft_uoff> & extent) const
{
    ft_uoff mem_offset = extent.second.user_data;
    ft_size mem_length = (ft_size) extent.second.length; // check for overflow?
    int err;
    if ((err = msync((char *)storage_mmap + mem_offset, mem_length, MS_SYNC)) != 0) {
        ff_log(FC_WARN, errno, "I/O error in %s msync(address + %"FT_ULL", length = %"FT_ULL")",
                label[FC_STORAGE], (ft_ull)mem_offset, (ft_ull)mem_length);
        err = 0;
    }
    return err;
}

/**
 * write zeroes to device (or to storage).
 * used to remove device-renumbered blocks once remapping is finished
 */
int fr_io_posix::zero_bytes(fr_to to, ft_uoff offset, ft_uoff length)
{
    static char * zero_buf = NULL;
    enum { ZERO_BUF_LEN = 1024*1024 };
    ft_uoff max = to == FC_TO_DEV ? dev_length() : (ft_uoff) storage_mmap_size;
    int err = 0;
    do {
        if (!ff_can_sum(offset, length) || length > max || offset > max - length) {
            err = ff_log(FC_FATAL, EOVERFLOW, "internal error! %s io.zero(to = %d, offset = %"FT_ULL", length = %"FT_ULL")"
                         " overflows maximum allowed %"FT_ULL,
                         label[to == FC_TO_DEV ? FC_DEVICE : FC_STORAGE],
                         (int)to, (ft_ull)offset, (ft_ull)length, (ft_ull)max);
            break;
        }
        if (ui() != NULL)
            ui()->show_io_write(to, offset, length);
        if (simulate_run())
            break;

        if (to == FC_TO_STORAGE) {
            memset((char *) storage_mmap + (ft_size)offset, '\0', (ft_size)length);
            break;
        }
        /* else (to == FC_TO_DEVICE) */

        if (zero_buf == NULL) {
            if ((zero_buf = (char *) malloc(ZERO_BUF_LEN)) == NULL)
                return ENOMEM;
            memset(zero_buf, '\0', ZERO_BUF_LEN);
        }
        int dev_fd = fd[FC_DEVICE];
        if ((err = ff_posix_lseek(dev_fd, offset)) != 0) {
            err = ff_log(FC_ERROR, err, "error in %s lseek(fd = %d, offset = %"FT_ULL")", label[FC_DEVICE], dev_fd, (ft_ull) offset);
            break;
        }
        ft_uoff chunk;
        while (length != 0) {
            chunk = ff_min2<ft_uoff>(length, ZERO_BUF_LEN);
            if ((err = ff_posix_write(dev_fd, zero_buf, chunk)) != 0) {
                err = ff_log(FC_ERROR, err, "error in %s write({fd = %d, offset = %"FT_ULL"}, zero_buffer, length = %"FT_ULL")",
                             label[FC_DEVICE], dev_fd, (ft_ull) offset, (ft_ull) chunk);
                break;
            }
            length -= chunk;
        }
    } while (0);
    return err;
}

/**
 * write zeroes to primary storage.
 * used to remove primary-storage once remapping is finished
 * and clean the remaped file-system
 */
int fr_io_posix::zero_primary_storage()
{
    fr_vector<ft_uoff>::const_iterator begin = primary_storage().begin(), iter, end = primary_storage().end();
    ft_size mem_offset, mem_length;

    const bool simulated = simulate_run();
    FT_UI_NS fr_ui * this_ui = ui();

    for (iter = begin; iter != end; ++iter) {
        const fr_extent<ft_uoff> & extent = *iter;
        mem_offset = extent.second.user_data;
        mem_length = (ft_size) extent.second.length; // check for overflow?

        if (this_ui != NULL)
            this_ui->show_io_write(FC_TO_STORAGE, mem_offset, mem_length);

        if (!simulated)
            memset((char *) storage_mmap + mem_offset, '\0', mem_length);
    }
    return 0;
}


FT_IO_NAMESPACE_END
