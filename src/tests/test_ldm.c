/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
#include "pho_test_utils.h"
#include "../ldm/ldm_common.h"
#include "pho_ldm.h"

static int _find_dev(const struct mntent *mntent, void *cb_data)
{
    const char *dev_name = cb_data;

    if (!strcmp(mntent->mnt_fsname, dev_name)) {
        pho_info("found device '%s': fstype='%s'", dev_name,
                 mntent->mnt_type);
        return 1;
    }

    return 0;
}

static int test_mnttab(void *arg)
{
    int rc;

    rc = mnttab_foreach(_find_dev, "proc");

    if (rc == 0)
        LOG_RETURN(-ENOENT, "proc not found");
    else if (rc == 1)
        /* expected return */
        return 0;

    /* other error */
    return -1;
}

static int test_df_0(void *arg)
{
    struct ldm_fs_space spc;

    return common_statfs("/tmp", &spc);
}

static int test_df_1(void *arg)
{
    struct ldm_fs_space spc;
    struct fs_adapter fsa;
    int rc;

    rc = get_fs_adapter(PHO_FS_POSIX, &fsa);
    if (rc)
        return rc;

    return ldm_fs_df(&fsa, "/tmp", &spc);
}

static int test_df_2(void *arg)
{
    struct ldm_fs_space spc;

    return common_statfs(NULL, &spc);
}

int main(int argc, char **argv)
{
    test_env_initialize();

    run_test("test mnttab", test_mnttab, NULL, PHO_TEST_SUCCESS);

    run_test("test df (direct call)", test_df_0, NULL, PHO_TEST_SUCCESS);
    run_test("test df (via fs_adapter)", test_df_1, NULL, PHO_TEST_SUCCESS);
    run_test("test df (NULL path)", test_df_2, NULL, PHO_TEST_FAILURE);

    pho_info("ldm_common: All tests succeeded");
    exit(EXIT_SUCCESS);
}
