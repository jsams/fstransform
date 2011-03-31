/*
 * job.cc
 *
 *  Created on: Mar 9, 2011
 *      Author: max
 */

#include "first.hh"

#include <cerrno>    // for ENOMEM
#include <cstdio>    // for sprintf()
#include <cstdlib>   // for malloc(), free(), getenv()
#include <cstring>   // for memcpy(), sprintf()

#include "io/util.hh"  // for ff_mkdir()
#include "job.hh"      // for ft_job
#include "log.hh"      // for ff_log*()


FT_NAMESPACE_BEGIN

/** default constructor */
ft_job::ft_job()
    : this_dir(), this_log_file(NULL), this_id(0),
      this_clear(FC_CLEAR_AUTODETECT), this_force_run(false), this_simulate_run(false)
{
    for (ft_size i = 0; i < FC_STORAGE_SIZE_N; i++)
        this_storage_size[i] = 0;
}

/** destructor. calls quit() */
ft_job::~ft_job()
{
    quit();
}

/** initialize logging subsystem, or return error */
int ft_job::init_log()
{
    std::string log_file_name = this_dir;
    log_file_name += "/fstransform.log";

    const char * log_file = log_file_name.c_str();

    if ((this_log_file = fopen(log_file, "a")) == NULL)
        return ff_log(FC_ERROR, errno, "failed to open log file '%s'", log_file);

    (void) setvbuf(this_log_file, NULL, _IOLBF, 0);

    /* note 1.4.3) fstransform.log always uses FC_FMT_DATETIME_LEVEL_CALLER_MSG */
    ff_log_register(this_log_file, FC_FMT_DATETIME_LEVEL_CALLER_MSG);
    return 0;
}


/** initialize this job, or return error */
int ft_job::init(const ft_args & args)
{
    const char * user_home = NULL;
    if (args.root_dir == NULL) {
        user_home = getenv("HOME");
        if (user_home != NULL) {
            this_dir = user_home;
            this_dir += '/';
        }
    } else {
        this_dir = args.root_dir;
        this_dir += '/';
    }
    this_dir += ".fstransform";

    const char * path = this_dir.c_str();

    if (args.root_dir == NULL && user_home == NULL)
        ff_log(FC_WARN, 0, "$HOME is not set, persistent storage will use sub-folders of '%s' in current directory", path);

    (void) FT_IO_NS ff_mkdir(path);

    this_dir += "/job.";
    ft_size len = this_dir.size();
    ft_uint i, job_min = 1, job_max = (ft_uint)-1;
    int err = 0;

    if (args.job_id != 0)
        /* force job_id */
        job_min = args.job_id, job_max = args.job_id + 1;

    path = this_dir.c_str();

    for (i = job_min; i != job_max; i++) {
        // 1 + 3*sizeof(ft_uint) chars are enough to safely print (ft_uint)
        this_dir.resize(len + 2 + 3*sizeof(ft_uint));
        sprintf(& this_dir[len], "%"FS_ULL, (ft_ull) i);
        this_dir.resize(len + strlen(& this_dir[len]));

        path = this_dir.c_str();

        if ((err = FT_IO_NS ff_mkdir(path)) == 0
                && (err = init_log()) == 0)
        {
            ff_log(FC_NOTICE, 0, "starting job %"FS_ULL, (ft_ull)i);
            ff_log(FC_INFO, 0, "job persistent data and logs will be in '%s'", path);
            break;
        }
    }
    if (i == job_max) {
        if (args.job_id != 0)
            err = ff_log(FC_ERROR, err, "failed to create persistent data folder '%s' for job id %"FS_ULL, path, (ft_ull) args.job_id);
        else
            err = ff_log(FC_ERROR, err, "failed to locate a free job id, tried range %"FS_ULL"...%"FS_ULL, (ft_ull) job_min, (ft_ull) (job_max-1));
    }
    if (err == 0) {
        for (ft_size l = 0; l < FC_STORAGE_SIZE_N; l++)
            this_storage_size[l] = args.storage_size[l];
        this_id = i;
        this_clear = args.job_clear;
        this_simulate_run = args.simulate_run;
    } else
        quit();

    return err;

}

void ft_job::quit()
{
    if (this_log_file != NULL) {
        ff_log_unregister(this_log_file);
        fclose(this_log_file);
        this_log_file = NULL;
    }
    this_dir.clear();
    for (ft_size i = 0; i < FC_STORAGE_SIZE_N; i++)
        this_storage_size[i] = 0;
    this_id = 0;
}

FT_NAMESPACE_END
