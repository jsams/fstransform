/*
 * work.t.hh
 *
 *  Created on: Feb 28, 2011
 *      Author: max
 */
#include "first.hh"

#include <cerrno>         // for errno, EFBIG
#include <cstdio>         // for fprintf(), stdout, stderr

#include "assert.hh"      // for ff_assert()
#include "log.hh"         // for ff_log()
#include "vector.hh"      // for ft_vector<T>
#include "map.hh"         // for ft_map<T>
#include "pool.hh"        // for ft_pool<T>
#include "util.hh"        // for ff_pretty_size()
#include "work.hh"        // for ff_dispatch(), ft_work<T>
#include "io/io.hh"       // for ft_io
#include "io/io_posix.hh" // for ft_io_posix
#include "arch/mem.hh"    // for ff_arch_mem_system_free()

FT_NAMESPACE_BEGIN

enum {
    FC_DEVICE = FT_IO_NS ft_io::FC_DEVICE,
    FC_LOOP_FILE = FT_IO_NS ft_io::FC_LOOP_FILE,
    FC_STORAGE = FT_IO_NS ft_io_posix::FC_STORAGE,
    FC_PRIMARY_STORAGE = FT_IO_NS ft_io_posix::FC_PRIMARY_STORAGE,
};

char const* const* const label = FT_IO_NS ft_io_posix::label;


template<typename T>
void ft_work<T>::show(const char * label, ft_uoff effective_block_size, const ft_map<T> & map, ft_log_level level)
{
    ft_log_level header_level = level <= FC_TRACE ? FC_DEBUG : level;

    if (!ff_log_is_enabled(header_level) && !ff_log_is_enabled(level))
        return;

    typename ft_map<T>::const_iterator iter = map.begin(), end = map.end();
    ft_size n = map.size();

    if (iter != end) {
        ff_log(header_level, 0, "# %4"FS_ULL" extent%s in %s, effective block size = %"FS_ULL,
               (ft_ull) n, (n == 1 ? " " : "s"), label, (ft_ull) effective_block_size);

        if (ff_log_is_enabled(level)) {
            ff_log(level, 0, "# extent \t\tphysical\t\t logical\t  length\tuser_data");

            for (ft_size i = 0; iter != end; ++iter, ++i) {
                ff_log(level, 0, "%8"FS_ULL"\t%12"FS_ULL"\t%12"FS_ULL"\t%8"FS_ULL"\t(%"FS_ULL")", (ft_ull)i,
                        (ft_ull) iter->first.physical,
                        (ft_ull) iter->second.logical,
                        (ft_ull) iter->second.length,
                        (ft_ull) iter->second.user_data);
            }
        }
    } else {
        ff_log(header_level, 0, "#   no extents in %s", label);
    }
    ff_log(level, 0, "");
}



/** default constructor */
template<typename T>
ft_work<T>::ft_work()
    : dev_map(), dev_free_map(), storage_map(), work_count(0)
{ }


/** destructor. calls quit() */
template<typename T>
ft_work<T>::~ft_work()
{
    quit();
}


/** performs cleanup. called by destructor, you can also call it explicitly after (or instead of) run()  */
template<typename T>
void ft_work<T>::quit()
{
    dev_map.clear();
    dev_free_map.clear();
    storage_map.clear();
    work_count = 0;
}



/**
 * high-level do-everything method. calls in sequence init(), run() and quit().
 * return 0 if success, else error.
 */
template<typename T>
int ft_work<T>::main(ft_vector<ft_uoff> & loop_file_extents,
                     ft_vector<ft_uoff> & free_space_extents, FT_IO_NS ft_io & io)
{
    ft_work<T> worker;
    return worker.run(loop_file_extents, free_space_extents, io);

    // worker.quit() is called automatically by destructor, no need to call explicitly
}

/** full transformation algorithm */
template<typename T>
int ft_work<T>::run(ft_vector<ft_uoff> & loop_file_extents,
                    ft_vector<ft_uoff> & free_space_extents, FT_IO_NS ft_io & io)
{
    int err;
    (err = init(io)) == 0
        && (err = analyze(loop_file_extents, free_space_extents)) == 0
        && (err = create_storage()) == 0
        && (err = relocate()) == 0;
    return err;
}

/**
 *  check if LOOP-FILE and DEVICE in-use extents can be represented
 *  by ft_map<T>. takes into account the fact that all extents
 *  physical, logical and length will be divided by effective block size
 *  before storing them into ft_map<T>.
 *
 *  return 0 for check passes, else error (usually EFBIG)
 */
template<typename T>
int ft_work<T>::check(const FT_IO_NS ft_io & io)
{
    ft_uoff eff_block_size_log2 = io.effective_block_size_log2();
    ft_uoff dev_length = io.dev_length();

    ft_uoff block_count = dev_length >> eff_block_size_log2;
    // possibly narrowing cast, let's check for overflow
    T n = (T) block_count;
    int err = 0;
    if (n < 0 || block_count != (ft_uoff) n)
        /* overflow! */
        err = EOVERFLOW;
    return err;
}

/**
 * call check(io) to ensure that io.dev_length() can be represented by T,
 * then checks that I/O is open.
 * if success, stores a reference to I/O object.
 */
template<typename T>
int ft_work<T>::init(FT_IO_NS ft_io & io)
{
    int err;
    do {
        if ((err = check(io)) != 0)
            break;
        if (!io.is_open()) {
            err = ENOTCONN; // I/O is not open !
            break;
        }
        this->io = & io;
    } while (0);

    return err;
}




static ft_size ff_mem_page_size()
{
    enum {
        FC_PAGE_SIZE_IF_UNKNOWN = 4096 // assume 4k (most common value) if cannot be detected
    };

    static ft_size page_size = 0;
    if (page_size == 0) {
        if ((page_size = FT_ARCH_NS ff_arch_mem_page_size()) == 0) {
            ff_log(FC_WARN, 0, "cannot detect system PAGE_SIZE. assuming 4 kilobytes and continuing, but troubles (mmap() errors) are very likely");
            page_size = FC_PAGE_SIZE_IF_UNKNOWN;
        }
    }
    return page_size;
}


template<typename T>
static T ff_round_up(T n, T power_of_2_minus_1)
{
	if (n & power_of_2_minus_1)
		n = (n | power_of_2_minus_1) + 1;
	return n;
}

/* trim extent on both ends to align it to page_size. return trimmed extent length (can be zero) */
template<typename T>
T ff_extent_align(typename ft_map<T>::value_type & extent, T page_size_blocks_m_1)
{
    T physical = extent.first.physical;
    T end = physical + extent.second.length;
    T new_physical = ff_round_up<T>(physical, page_size_blocks_m_1);
    T new_end = end & ~page_size_blocks_m_1;
    if (new_end <= new_physical)
        return extent.second.length = 0;

    extent.first.physical = new_physical;
    extent.second.logical += new_physical - physical;
    return extent.second.length = new_end - new_physical;
}


/**
 * analysis phase of transformation algorithm,
 * must be executed before create_storage() and relocate()
 *
 * given LOOP-FILE extents and FREE-SPACE extents as ft_vectors<ft_uoff>,
 * compute LOOP-FILE extents map and DEVICE in-use extents map
 *
 * assumes that vectors are ordered by extent->logical, and modifies them
 * in place: vector contents will be UNDEFINED when this method returns.
 *
 * implementation: to compute this->dev_map, performs in-place the union of specified
 * loop_file_extents and free_space_extents, then sorts in-place and complements such union.
 */
template<typename T>
int ft_work<T>::analyze(ft_vector<ft_uoff> & loop_file_extents,
                        ft_vector<ft_uoff> & free_space_extents)
{
    // cleanup in case dev_map, dev_free_map or storage_map are not empty, or work_count != 0
    quit();

    ft_map<T> loop_map, loop_holes_map, renumbered_map;

    ft_uoff eff_block_size_log2 = io->effective_block_size_log2();
    ft_uoff eff_block_size      = (ft_uoff)1 << eff_block_size_log2;
    ft_uoff dev_length          = io->dev_length();
    /*
     * algorithm: 1) find LOOP-FILE (logical) holes, i.e. LOOP-HOLES,
     * and store them in holes_map
     * note: all complement maps have physical == logical
     */
    loop_holes_map.complement0_logical_shift(loop_file_extents, eff_block_size_log2, dev_length);




    /* algorithm: 0) compute LOOP-FILE extents and store in loop_map, sorted by physical */
    loop_file_extents.sort_by_physical();
    loop_map.append0_shift(loop_file_extents, eff_block_size_log2);
    /* show LOOP-FILE extents sorted by physical */
    show(label[FC_LOOP_FILE], eff_block_size, loop_map);


    /* algorithm: 0) compute FREE-SPACE extents and store in dev_free_map, sorted by physical
     *
     * we must manually set ->logical = ->physical for all free_space_extents:
     * here dev_free_map is just free space, but for I/O that computed it
     * it could have been a ZERO-FILE with its own ->logical,
     *
     * note: changing ->logical may also allow merging extents!
     */
    {
        ft_vector<ft_uoff>::const_iterator iter = free_space_extents.begin(), end = free_space_extents.end();
        T physical, length;
        for (; iter != end; ++iter) {
            physical = iter->first.physical >> eff_block_size_log2;
            length = iter->second.length >> eff_block_size_log2;
            dev_free_map.insert(physical, physical, length, FC_DEFAULT_USER_DATA);
        }
        show("free-space", eff_block_size, dev_free_map);
    }





    /* algorithm: 0) compute DEVICE extents
     *
     * how: compute physical complement of all LOOP-FILE and FREE-SPACE extents
     * and assume they are used by DEVICE for its file-system
     */
    /* compute in-place the union of LOOP-FILE extents and FREE-SPACE extents */
    loop_file_extents.append_all(free_space_extents);
    /* sort the union by physical: needed by dev_map.complement0_physical_shift() immediately below */
    loop_file_extents.sort_by_physical();
    dev_map.complement0_physical_shift(loop_file_extents, eff_block_size_log2, dev_length);
    /* show DEVICE extents sorted by physical */
    show(label[FC_DEVICE], eff_block_size, dev_map);




    /*
     * algorithm: 2), 3) allocate LOOP-HOLES for DEVICE extents logical destination
     * and for LOOP-FILE invariant extents
     */
    /* show LOOP-HOLES extents before allocation, sorted by physical */
    show("initial loop-holes", eff_block_size, loop_holes_map);

    /* algorithm: 2) re-number used DEVICE blocks, setting ->logical to values
     * from LOOP-HOLES. do not greedily use low hole numbers:
     * a) prefer holes with ->logical numbers equal to DEVICE ->physical block number:
     *    they produce an INVARIANT block, already in its final destination
     *    (marked with @@)
     * b) spread the remaining ->logical across rest of holes (use best-fit allocation)
     */
    /* how: intersect dev_map and loop_holes_map and put result into renumbered_map */
    renumbered_map.intersect_all_all(dev_map, loop_holes_map);
    /* show DEVICE INVARIANT extents (i.e. already in their final destination), sorted by physical */
    show("device (invariant)", eff_block_size, renumbered_map);
    /* remove from dev_map all the INVARIANT extents in renumbered_map */
    dev_map.remove_all(renumbered_map);
    /*
     * also remove from loop_holes_map all extents in renumbered_map
     * reason: they are no longer free (logical) holes,
     * since we allocated them for DEVICE INVARIANT extents
     */
    loop_holes_map.remove_all(renumbered_map);
    /*
     * then clear renumbered_map: its extents are already in their final destination
     * (they are INVARIANT) -> no work on them
     */
    renumbered_map.clear();
    /* show LOOP-HOLES (sorted by physical) after allocating DEVICE-INVARIANT extents */
    show("loop-holes after device (invariant)", eff_block_size, loop_holes_map);



    /*
     * algorithm: 2) b) spread the remaining DEVICE ->logical across rest of LOOP-HOLES
     * (use best-fit allocation)
     */
    /* order loop_holes_map by length */
    ft_pool<T> loop_holes_pool(loop_holes_map);
    /*
     * allocate LOOP-HOLES extents to store DEVICE extents using a best-fit strategy.
     * move allocated extents from dev_map to renumbered_map
     */
    loop_holes_pool.allocate_all(dev_map, renumbered_map);
    /* show DEVICE RENUMBERED extents sorted by physical */
    show("device (renumbered)", eff_block_size, renumbered_map);
    /* show LOOP-HOLES extents after allocation, sorted by physical */
    show("final loop-holes", eff_block_size, loop_holes_map);

    /* sanity check */
    if (!dev_map.empty()) {
        ff_log(FC_FATAL, 0, "internal error: there are extents in DEVICE not fitting DEVICE. this is impossible! I give up");
        /* show DEVICE-NOTFITTING extents sorted by physical */
        show("device (not fitting)", eff_block_size, dev_map, FC_NOTICE);
        return ENOSPC;
    }
    /* move DEVICE (RENUMBERED) back into dev_map and clear renumbered_map */
    dev_map.swap(renumbered_map);




    /*
     * 2.1) mark as INVARIANT (with @@) the (logical) extents in LOOP-FILE
     * already in their final destination, and forget them (no work on those).
     * also compute total length of extents remaining in LOOP-FILE and store in work_count.
     */
    map_iterator iter = loop_map.begin(), tmp, end = loop_map.end();
    work_count = 0; /**< number of blocks to be relocated */

    while (iter != end) {
        if (iter->first.physical == iter->second.logical) {
            /* move INVARIANT extents to renumbered_map, to show them later */
            renumbered_map.insert(*iter);
            tmp = iter;
            ++iter;
            /* forget INVARIANT extents (i.e. remove from loop_map) */
            loop_map.remove(tmp);
        } else {
            /* not INVARIANT, compute loop_map length... */
            work_count += iter->second.length;
            /*
             * also prepare for item 3) "merge renumbered DEVICE extents with remaining LOOP-FILE extents"
             * i.e. remember who's who
             */
            iter->second.user_data = FC_LOOP_FILE;
            ++iter;
        }
    }
    /* show LOOP-FILE (INVARIANT) blocks, sorted by physical */
    show("loop-file (invariant)", eff_block_size, renumbered_map);
    /* then forget them */
    renumbered_map.clear();






    /*
     * algorithm: 3) merge renumbered DEVICE extents with LOOP-FILE blocks (remember who's who)
     * also compute total length of extents remaining in DEVICE and add it to work_count.
     */
    iter = dev_map.begin();
    end = dev_map.end();
    for (; iter != end; ++iter) {
        work_count += iter->second.length;
        iter->second.user_data = FC_DEVICE;
        loop_map.insert0(iter->first, iter->second);
    }
    dev_map.clear();
    /*
     * from now on, we only need one of dev_map or loop_map, not both.
     * we choose dev_map: more intuitive name, and already stored in 'this'
     */
    dev_map.swap(loop_map);
    dev_map.total_count(work_count);
    dev_map.used_count(work_count);
    /* show DEVICE + LOOP-FILE extents after merge, sorted by physical */
    show("device + loop-file (merged)", eff_block_size, dev_map);

    double pretty_len = 0.0;
    const char * pretty_unit = ff_pretty_size((ft_uoff) work_count << eff_block_size_log2, & pretty_len);

    ff_log(FC_NOTICE, 0, "analysis completed: %.2f %sbytes must be relocated", pretty_len, pretty_unit);

    /*
     * algorithm: 4) compute (physical) intersection of FREE-SPACE and LOOP-HOLES,
     * and mark it as FREE-SPACE (INVARIANT) (with !!).
     * we can use these extents as partial or total replacement for STORAGE - see 5)
     * if they are relatively large (see below for meaning of "relatively large")
     *
     * forget the rest of LOOP-HOLES extents, we will not need them anymore
     */
    /* how: intersect dev_free_map and loop_holes_map and put result into renumbered_map */
    renumbered_map.intersect_all_all(dev_free_map, loop_holes_map);
    /* then discard extents smaller than either work_count / 1024 or page_size*/

    /* page_size_blocks = number of blocks in one RAM page. will be zero if page_size < block_size */
    const T page_size_blocks = (T) (ff_mem_page_size() >> eff_block_size_log2);

    /* consider for PRIMARY-STORAGE only "relatively large" blocks, i.e.
     * 1) at least 4096 * PAGE_SIZE bytes long, or at least work_count / 1024 blocks long
     * 2) in any case, at least 1 * PAGE_SIZE bytes long
     */
    ft_uoff hole_threshold = ff_min2((ft_uoff) work_count >> 10, (ft_uoff) page_size_blocks << 12);
    T hole_len, hole_total_len = 0;

    iter = renumbered_map.begin();
    end = renumbered_map.end();
    show("free-space (invariant)", eff_block_size, renumbered_map);
    while (iter != end) {
        if ((ft_uoff) iter->second.length >= hole_threshold) {
            /* trim hole on both ends to align it to PAGE_SIZE */
            if (page_size_blocks <= 1 || (ft_uoff) (hole_len = ff_extent_align(*iter, page_size_blocks - 1)) >= hole_threshold) {
                /* hole is large enough to be useful */
                hole_total_len += hole_len;
                ++iter;
                continue;
            }
        }
        tmp = iter;
        ++iter;
        renumbered_map.remove(tmp);
    }
    /*
     * move FREE-SPACE (INVARIANT) extents into dev_free_map (i.e. PRIMARY-STORAGE),
     * as the latter is stored into 'this'
     */
    dev_free_map.swap(renumbered_map);
    /* show PRIMARY-STORAGE extents, sorted by physical */
    show("primary-storage (= free-space, invariant, contiguous, aligned)", eff_block_size, dev_free_map);


    pretty_len = 0.0;
    pretty_unit = ff_pretty_size((ft_uoff) hole_total_len << eff_block_size_log2, & pretty_len);
    ft_size dev_free_map_n = dev_free_map.size();

    ff_log(FC_INFO, 0, "%s: located %.2f %sbytes (%"FS_ULL" fragment%s) usable in %s (free, invariant, contiguous and aligned)",
           label[FC_PRIMARY_STORAGE], pretty_len, pretty_unit, (ft_ull)dev_free_map_n, (dev_free_map_n == 1 ? "" : "s"), label[FC_DEVICE]);

    dev_free_map.total_count(hole_total_len);

    return 0;
}


static int unusable_storage_size(ft_uoff requested_len, const char * type_descr, ft_ull type_bytes, const char * msg)
{
	ff_log(FC_FATAL, 0, "fatal error: cannot reuse job %s size %"FS_ULL" bytes: this %s is %"FS_ULL" bytes,"
			" original job was probably created on a platform with %s",
			label[FC_STORAGE], (ft_ull) requested_len, type_descr, type_bytes, msg);
	/* mark error as reported */
	return -EOVERFLOW;
}


/**
 * creates on-disk secondary storage, used as (small) backup area during relocate().
 * must be executed before relocate()
 */
template<typename T>
int ft_work<T>::create_storage()
{
    enum {
        _1M_minus_1 = 1024*1024 - 1,
        _64k_minus_1 = 64*1024 - 1,
    };

	const ft_uoff eff_block_size_log2 = io->effective_block_size_log2();
	const ft_uoff eff_block_size_minus_1 = ((ft_uoff)1 << eff_block_size_log2) - 1;

	const ft_uoff free_ram = FT_ARCH_NS ff_arch_mem_system_free();
	const ft_uoff page_size_minus_1 = (ft_uoff) ff_mem_page_size() - 1;

	ft_uoff primary_len = (ft_uoff) dev_free_map.total_count() << eff_block_size_log2;
	ft_uoff total_len = (ft_uoff) io->job_storage_size(), requested_len = total_len;
	bool exact = io->job_storage_size_exact();

	if (exact && requested_len == 0) {
		ff_log(FC_FATAL, 0, "fatal error: resumed job STORAGE is 0 bytes. impossible!");
		return -EINVAL;
	}

	if (total_len != 0) {
		/* honor requested storage size, but check for possible problems */

		double free_pretty_len = 0.0;
		const char * free_pretty_unit = ff_pretty_size(free_ram, & free_pretty_len);

		if (free_ram == 0) {
			ff_log(FC_WARN, 0, "cannot detect free RAM amount."
					" no idea if the %.2f %sbytes requested for mmapped() %s will fit into free RAM."
					" continuing, but troubles (memory exhaustion) are possible",
					free_pretty_len, free_pretty_unit, label[FC_STORAGE]);

		} else if (total_len > free_ram / 3 * 2) {
			double total_pretty_len = 0.0;
			const char * total_pretty_unit = ff_pretty_size(total_len, & total_pretty_len);

			ff_log(FC_WARN, 0, "requested %.2f %sbytes for mmapped() %s, but only %.2f %sbytes RAM are free."
					" honoring the request, but expect troubles (memory exhaustion)",
					total_pretty_len, total_pretty_unit, label[FC_STORAGE], free_pretty_len, free_pretty_unit);
		}
	} else {
		/*
		 * auto-detect total storage size to use:
		 * we want it to be the smallest between
		 *   33% of free RAM (if free RAM cannot be determined, use 16 MB if 32bit platform, else use 256MB)
		 *   10% of bytes to relocate
		 */
		ft_uoff free_ram_3;

		if (free_ram != 0) {
			free_ram_3 = (free_ram + 2) / 3;
		} else {
			free_ram_3 = sizeof(ft_size) <= 4 ? (ft_uoff) 16*1024*1024 : (ft_uoff) 256*1024*1024;

			double free_pretty_len = 0.0;
			const char * free_pretty_unit  = ff_pretty_size(free_ram_3 * 3,  & free_pretty_len);

			ff_log(FC_WARN, 0, "cannot detect free RAM amount. assuming at least %.2f %sbytes RAM are free."
					" expect troubles (memory exhaustion) if not true",
					free_pretty_len, free_pretty_unit);
		}
		ft_uoff work_length_10 = (((ft_uoff) work_count << eff_block_size_log2) + 9) / 10;
		total_len = ff_min2(free_ram_3, work_length_10);

		/* round up to multiples of 1M */
		total_len = ff_round_up<ft_uoff>(total_len, _1M_minus_1);
	}

	/* round up total_len to a multiple of PAGE_SIZE */
	total_len = ff_round_up<ft_uoff>(total_len, page_size_minus_1);
	if (exact && total_len != requested_len)
		return unusable_storage_size(requested_len, "system PAGE_SIZE", (ft_ull)(page_size_minus_1 + 1), "smaller RAM page size" );

	/* round up total_len to a multiple of effective block size */
	total_len = ff_round_up<ft_uoff>(total_len, eff_block_size_minus_1);
	if (exact && total_len != requested_len)
		return unusable_storage_size(requested_len, "device effective block size", (ft_ull)(eff_block_size_minus_1 + 1), "smaller file-system block size" );

	const ft_uoff aligment_size_minus_1 = eff_block_size_minus_1 | page_size_minus_1;

	/* round down primary_len to a multiple of PAGE_SIZE */
	/* round down primary_len to a multiple of effective block size */
	primary_len &= ~aligment_size_minus_1;

	/*
	 * adjust both total_len and primary_len as follows:
	 * truncate to fit off_t (== ft_off, signed version of ft_uoff)
	 * truncate to 1/4 of addressable RAM (= 1GB on 32-bit machines), or to whole addressable RAM if job_storage_size_exact()
	 * keep alignment to PAGE_SIZE and effective block size!
	 */
	const ft_uoff off_max = ((ft_uoff)(ft_off)-1 >> 1) & ~aligment_size_minus_1;
	primary_len = ff_min2<ft_uoff>(primary_len, off_max);
	total_len =   ff_min2<ft_uoff>(total_len,   off_max);
	if (exact && total_len != requested_len)
		return unusable_storage_size(requested_len, "system (off_t)", (ft_ull)sizeof(ft_size), "larger maximum file size");

	const ft_size mem_max = (((ft_uoff)(ft_size)-1 >> (exact ? 0 : 2)) + 1) & ~aligment_size_minus_1;
	primary_len = ff_min2<ft_uoff>(primary_len, mem_max);
	total_len   = ff_min2<ft_uoff>(total_len,   mem_max);
	if (exact && total_len != requested_len)
		return unusable_storage_size(requested_len, "system (size_t)", (ft_ull)sizeof(ft_size), "larger addressable memory");

	if (total_len == 0) {
		total_len = aligment_size_minus_1 + 1;

		double total_pretty_len = 0.0;
		const char * total_pretty_unit = ff_pretty_size(total_len, & total_pretty_len);

		ff_log(FC_WARN, 0, "%s size to use would be 0 bytes, increasing to %.2f %sbytes",
				label[FC_STORAGE], total_pretty_len, total_pretty_unit);
	}


	if (primary_len > total_len)
		primary_len = total_len;
	ft_uoff secondary_len = total_len - primary_len;

	/* remember storage_size in case this job is resumed later */
	io->job_storage_size((ft_size) total_len);

	/* fill io->primary_storage() with PRIMARY-STORAGE extents actually used */
	fill_io_primary_storage(primary_len);

	double pretty_len = 0.0;
	const char * pretty_unit = ff_pretty_size(primary_len, & pretty_len);
	ft_size fragment_n = io->primary_storage().size();

	ff_log(FC_INFO, 0, "%s: actually using %.2f %sbytes (%"FS_ULL" fragment%s) from %s",
		   label[FC_PRIMARY_STORAGE], pretty_len, pretty_unit,
		   (ft_ull)fragment_n, (fragment_n == 1 ? "" : "s"), label[FC_DEVICE]);

	show("primary-storage (actually used)", (ft_uoff) 1 << eff_block_size_log2, dev_free_map);

	return io->create_storage(secondary_len);
}

/**
 * fill io->primary_storage() with DEVICE extents to be actually used as PRIMARY-STORAGE
 * (already computed into dev_free_map by analyze()).
 *
 * if only a fraction of available PRIMARY-STORAGE will be actually used,
 * exploit a ft_pool<T> to select the largest contiguous extents.
 *
 * updates dev_free_map to contain the PRIMARY-STORAGE extents actually used.
 */
template<typename T>
void ft_work<T>::fill_io_primary_storage(ft_uoff primary_len)
{
	const ft_uoff eff_block_size_log2 = io->effective_block_size_log2();
	const ft_uoff eff_block_size_minus_1 = ((ft_uoff)1 << eff_block_size_log2) - 1;

    ff_assert((primary_len & eff_block_size_minus_1) == 0);

    /* first, copy all extents from dev_free_map to primary_storage */
    ft_vector<ft_uoff> & primary_storage = io->primary_storage();
    T physical, length;
    map_iterator map_iter = dev_free_map.begin(), map_end = dev_free_map.end();
	for (; map_iter != map_end; ++map_iter) {
		typename ft_map<T>::value_type & extent = *map_iter;
		physical = (ft_uoff) extent.first.physical << eff_block_size_log2;
		length   = (ft_uoff) extent.second.length  << eff_block_size_log2;

		primary_storage.append(physical, physical, length, extent.second.user_data);
	}

	/* then check: if not all extents will be actually used, drop the smallest ones */
	const ft_uoff available_len = (ft_uoff) dev_free_map.total_count() << eff_block_size_log2;
    if (available_len > primary_len) {
    	ft_uoff extra_len = available_len - primary_len;

    	/* sort by reverse length */
    	primary_storage.sort_by_reverse_length();

    	/*
    	 * iterate dropping the last (smallest) extents until we exactly reach primary_len.
    	 * (one final extent may be shrank instead of dropped)
    	 */
    	while (extra_len != 0 && !primary_storage.empty()) {
    		ft_extent<ft_uoff> & extent = primary_storage.back();
    		length   = extent.length();
    		if (length <= extra_len) {
    			// completely drop this extent
    			extra_len -= length;
    			primary_storage.pop_back();
    		} else {
    			// shrink this extent and break
    			extent.length() -= extra_len;
    			extra_len = 0;
    		}
    	}
    	primary_storage.sort_by_physical();
    	dev_free_map.clear();
    	dev_free_map.append0_shift(primary_storage, eff_block_size_log2);
    }

    dev_free_map.total_count((T)(primary_len >> eff_block_size_log2));
}


/** core of transformation algorithm, actually moves DEVICE blocks */
template<typename T>
int ft_work<T>::relocate()
{
    int err = 0;
    do {

    } while (0);
    return err;
}


FT_NAMESPACE_END