/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 *  vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 * Copyright (C) 2016 Seagate Technology LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the CeCILL License.
 *
 * The fact that you are presently reading this means that you have had
 * knowledge of the CeCILL license (http://www.cecill.info) and that you
 * accept its terms.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "Basic.h"

#include <pthread.h>
#include <sys/time.h>

#include "chglog_reader.h"
#include "list.h"
#include "entry_proc_hash.h"
#include "Memory.h"
#include "entry_processor.h"
#include "rbh_logs.h"
#include "rbh_modules.h"

#include <lustre/lustreapi.h>

#if HAVE_DECL_CLF_RENAME
    #define PROCESS_LOG_ONE_REC CLF_RENAME
#elif HAVE_CHANGELOG_EXTEND_REC
    #define PROCESS_LOG_ONE_REC CLF_EXT_VERSION
#else
    #define PROCESS_LOG_ONE_REC 0
#endif
#define TEST_LUSTRE_FID_OID 400000
#define TEST_LUSTRE_FID_SEQ 1

extern chglog_reader_config_t cl_reader_config;

static struct reader_thr_info_t thread_info;

#define CHANGELOG_RECORD(_rt, _idx, _tfid, _pfid, _name,                   \
                         _sfid, _spfid, _sname)                            \
{                                                                          \
    .rec_type = (_rt), .idx = (_idx), .tfid = (_tfid), .pfid = (_pfid),    \
    .name = (_name), .sfid = (_sfid), .spfid = (_spfid), .sname = (_sname) \
}

#define SHORT_RECORD(_rt, _idx, _tfid, _pfid, _name)           \
    CHANGELOG_RECORD((_rt), (_idx), (_tfid), (_pfid), (_name), \
                     zero_fid, zero_fid, NULL)

#define RENAME_RECORD(_idx, _pfid, _name, _sfid, _spfid, _sname)    \
    CHANGELOG_RECORD(CL_RENAME, (_idx), zero_fid, (_pfid), (_name), \
                     (_sfid), (_spfid), (_sname))

struct changelog_record_descr {
    uint32_t    rec_type;
    uint64_t    idx;
    entry_id_t  tfid;
    entry_id_t  pfid;
    const char *name;
    entry_id_t  sfid;
    entry_id_t  spfid;
    const char *sname;
};

void changelog_test_init(void);
void changelog_test_fini(void);

#define UNIT_TEST(test_name)  \
    void test_name(void); \
    void test_name(void)

#define UNIT_TEST_INFO(test_name) \
{#test_name, (test_name)}

UNIT_TEST(process_log_rec_invalid_test)
{
    CL_REC_TYPE rec;
    int         rc;

    rec.cr_type = -1;
    rc = process_log_rec(&thread_info, &rec);
    CU_ASSERT_EQUAL(rc, EINVAL);

    rec.cr_type = CL_LAST;
    rc = process_log_rec(&thread_info, &rec);
    CU_ASSERT_EQUAL(rc, EINVAL);
}

UNIT_TEST(process_log_rec_ignore_test)
{
    CL_REC_TYPE              rec;
    int                      rc;

    thread_info.suppressed_records = 0;
    thread_info.interesting_records = 0;

    /* Robinhood always ignores CL_MARK records. See definition of
     * record_filters in src/chglog_reader/chglog_reader.c
     **/
    rec.cr_type = CL_MARK;
    rc = process_log_rec(&thread_info, &rec);
    CU_ASSERT_EQUAL(rc, 0);
    CU_ASSERT_EQUAL(thread_info.suppressed_records, 1);
    CU_ASSERT_EQUAL(thread_info.interesting_records, 0);
}

static int process_log_rec_record_count(void)
{
    int                  n_records = 0;
    int                  i;
    struct id_hash_slot *slot;
    entry_proc_op_t     *op;

    for (i = 0; i < thread_info.id_hash->hash_size; ++i)
    {
        slot = thread_info.id_hash->slot + i;
        rh_list_for_each_entry(op, &slot->list, id_hash_list)
            ++n_records;
    }

    return n_records;
}

static void process_log_rec_queue_cleanup(void)
{
    entry_proc_op_t *op;

    while(!rh_list_empty(&thread_info.op_queue)) {
        op = rh_list_first_entry(&thread_info.op_queue,
                                 entry_proc_op_t, list);
        rh_list_del(&op->list);
        rh_list_del(&op->id_hash_list);

        EntryProcessor_Release(op);

        thread_info.op_queue_count --;
    }
}

#if defined(HAVE_CHANGELOG_EXTEND_REC) || defined(HAVE_FLEX_CL)

UNIT_TEST(process_log_rec_rename_one_zero_tfid_test)
{
#define TEST_SHORT_NAME "one_zero_tfid_test"
    CL_REC_TYPE             *rec;
    int                      rc;
    int                      n_records;
    const char               source_name[]= TEST_SHORT_NAME "_source";
    const char               target_name[]= TEST_SHORT_NAME "_target";
    size_t                   record_size;

    record_size = sizeof(CL_REC_TYPE) +
#if defined(HAVE_FLEX_CL)
                  sizeof(struct changelog_ext_rename) +
#endif
                  strlen(source_name) + strlen(target_name) + 2;
    rec = (CL_REC_TYPE*)MemAlloc(record_size);
    memset(rec, 0, record_size);
    CU_ASSERT_NOT_EQUAL_FATAL(rec, NULL);

    rec->cr_type = CL_RENAME;
    rec->cr_flags = PROCESS_LOG_ONE_REC;
    rec->cr_tfid.f_oid = 0;
    rec->cr_tfid.f_seq = 0;

    sprintf(rh_get_cl_cr_name(rec), "%s", target_name);
    sprintf((char*)changelog_rec_sname(rec), "%s", source_name);

    rec->cr_namelen = strlen(target_name) + strlen(source_name) + 1;

    rc = process_log_rec(&thread_info, rec);
    CU_ASSERT_EQUAL(rc, 0);
    CU_ASSERT_EQUAL(thread_info.suppressed_records, 0);
    CU_ASSERT_EQUAL(thread_info.interesting_records, 1);

    n_records = process_log_rec_record_count();
    CU_ASSERT_EQUAL(n_records, 2);

    MemFree(rec);

#undef TEST_SHORT_NAME
}

UNIT_TEST(process_log_rec_rename_non_zero_tfid_test)
{
#define TEST_SHORT_NAME "non_zero_tfid_test"

    CL_REC_TYPE             *rec;
    int                      rc;
    int                      n_records;
    const char               source_name[]= TEST_SHORT_NAME "_source";
    const char               target_name[]= TEST_SHORT_NAME "_target";
    size_t                   record_size;

    record_size = sizeof(CL_REC_TYPE) +
#if defined(HAVE_FLEX_CL)
                  sizeof(struct changelog_ext_rename) +
#endif
                  strlen(source_name) + strlen(target_name) + 2;
    rec = (CL_REC_TYPE*)MemAlloc(record_size);
    memset(rec, 0, record_size);
    CU_ASSERT_NOT_EQUAL_FATAL(rec, NULL);

    rec->cr_type = CL_RENAME;
    rec->cr_flags = PROCESS_LOG_ONE_REC;
    rec->cr_tfid.f_oid = TEST_LUSTRE_FID_OID;
    rec->cr_tfid.f_seq = TEST_LUSTRE_FID_SEQ;

    sprintf(rh_get_cl_cr_name(rec), "%s", target_name);
    sprintf((char*)changelog_rec_sname(rec), "%s", source_name);

    rec->cr_namelen = strlen(target_name) + strlen(source_name) + 1;

    rc = process_log_rec(&thread_info, rec);
    CU_ASSERT_EQUAL(rc, 0);
    CU_ASSERT_EQUAL(thread_info.suppressed_records, 0);
    CU_ASSERT_EQUAL(thread_info.interesting_records, 1);

    n_records = process_log_rec_record_count();
    CU_ASSERT_EQUAL(n_records, 3);

    MemFree(rec);

#undef TEST_SHORT_NAME
}
#endif

UNIT_TEST(process_log_rec_rename_ext_rec_one_zero_tfid_test)
{
#define TEST_SHORT_NAME "ext_rec_one_zero_tfid_test"
    CL_REC_TYPE             *rec;
    CL_REC_TYPE             *ext_rec;
    int                      rc;
    int                      n_records;

#if defined(HAVE_FLEX_CL)
    rec = (CL_REC_TYPE*)MemAlloc(sizeof(CL_REC_TYPE) +
                                 sizeof(struct changelog_ext_rename));
#else
    rec = (CL_REC_TYPE*)MemAlloc(sizeof(CL_REC_TYPE));
#endif
    CU_ASSERT_NOT_EQUAL_FATAL(rec, NULL);

    rec->cr_type = CL_RENAME;
    rec->cr_flags = 0;
    rec->cr_tfid.f_oid = 0;
    rec->cr_tfid.f_seq = 0;

    /* We don't need source and target file names for this test. */
    rec->cr_namelen = 0;

    rc = process_log_rec(&thread_info, rec);
    CU_ASSERT_EQUAL(rc, 0);
    CU_ASSERT_EQUAL(thread_info.suppressed_records, 0);
    CU_ASSERT_EQUAL(thread_info.interesting_records, 1);

    n_records = process_log_rec_record_count();
    CU_ASSERT_EQUAL(n_records, 0);

    ext_rec = (CL_REC_TYPE*)MemAlloc(sizeof(CL_REC_TYPE));
    CU_ASSERT_NOT_EQUAL_FATAL(ext_rec, NULL);
    memset(ext_rec, 0, sizeof(*ext_rec));

    ext_rec->cr_type = CL_EXT;
    ext_rec->cr_tfid.f_oid = 0;
    ext_rec->cr_tfid.f_seq = 0;
    ext_rec->cr_namelen = 0;

    rc = process_log_rec(&thread_info, ext_rec);
    CU_ASSERT_EQUAL(rc, 0);
    CU_ASSERT_EQUAL(thread_info.suppressed_records, 0);
    CU_ASSERT_EQUAL(thread_info.interesting_records, 2);

    n_records = process_log_rec_record_count();
    CU_ASSERT_EQUAL(n_records, 2);

    MemFree(rec);
    MemFree(ext_rec);

#undef TEST_SHORT_NAME
}

UNIT_TEST(process_log_rec_rename_ext_rec_non_zero_tfid_test)
{
#define TEST_SHORT_NAME "ext_rec_non_zero_tfid_test"
    CL_REC_TYPE             *rec;
    CL_REC_TYPE             *ext_rec;
    int                      rc;
    int                      n_records;
    const char               source_name[]= TEST_SHORT_NAME "_source";
    const char               target_name[]= TEST_SHORT_NAME "_target";
    size_t                   record_size;

#if defined(HAVE_FLEX_CL)
    rec = (CL_REC_TYPE*)malloc(sizeof(CL_REC_TYPE) +
                               sizeof(struct changelog_ext_rename));
#else
    rec = (CL_REC_TYPE*)malloc(sizeof(CL_REC_TYPE));
#endif
    CU_ASSERT_NOT_EQUAL_FATAL(rec, NULL);

    rec->cr_type = CL_RENAME;
    rec->cr_flags = 0;
    rec->cr_tfid.f_oid = TEST_LUSTRE_FID_OID;
    rec->cr_tfid.f_seq = TEST_LUSTRE_FID_SEQ;

    /* Have no need to have source and target file names here. */
    rec->cr_namelen = 0;

    rc = process_log_rec(&thread_info, rec);
    CU_ASSERT_EQUAL(rc, 0);
    CU_ASSERT_EQUAL(thread_info.suppressed_records, 0);
    CU_ASSERT_EQUAL(thread_info.interesting_records, 1);

    n_records = process_log_rec_record_count();
    CU_ASSERT_EQUAL(n_records, 0);

    record_size = sizeof(CL_REC_TYPE) +
#if defined(HAVE_FLEX_CL)
                  sizeof(struct changelog_ext_rename) +
#endif
                  strlen(source_name) + strlen(target_name) + 2;
    ext_rec = (CL_REC_TYPE*)MemAlloc(record_size);
    memset(ext_rec, 0, record_size);
    CU_ASSERT_NOT_EQUAL_FATAL(ext_rec, NULL);

    ext_rec->cr_type = CL_EXT;
    ext_rec->cr_flags = PROCESS_LOG_ONE_REC;
    ext_rec->cr_tfid.f_oid = TEST_LUSTRE_FID_OID;
    ext_rec->cr_tfid.f_seq = TEST_LUSTRE_FID_SEQ;

    sprintf(rh_get_cl_cr_name(ext_rec), "%s", target_name);
    sprintf((char*)changelog_rec_sname(ext_rec), "%s", source_name);

    ext_rec->cr_namelen = 0;

    rc = process_log_rec(&thread_info, ext_rec);
    CU_ASSERT_EQUAL(rc, 0);
    CU_ASSERT_EQUAL(thread_info.suppressed_records, 0);
    CU_ASSERT_EQUAL(thread_info.interesting_records, 2);

    n_records = process_log_rec_record_count();
    CU_ASSERT_EQUAL(n_records, 3);

    MemFree(rec);
    MemFree(ext_rec);

#undef TEST_SHORT_NAME
}

UNIT_TEST(process_log_rec_unlink)
{
#define TEST_SHORT_NAME "unlink"
    CL_REC_TYPE *rec;
    int          rc;
    int          n_records;
    const char   target_name[] = TEST_SHORT_NAME "_target";
    size_t       record_size;

    record_size = sizeof(CL_REC_TYPE) +
                  strlen(target_name) + 1;
    rec = (CL_REC_TYPE*)MemAlloc(record_size);
    CU_ASSERT_NOT_EQUAL_FATAL(rec, NULL);
    memset(rec, 0, record_size);

    rec->cr_type = CL_UNLINK;
    rec->cr_tfid.f_oid = TEST_LUSTRE_FID_OID;
    rec->cr_tfid.f_seq = TEST_LUSTRE_FID_SEQ;

    sprintf(rh_get_cl_cr_name(rec), "%s", target_name);
    rec->cr_namelen = strlen(target_name);

    rc = process_log_rec(&thread_info, rec);
    CU_ASSERT_EQUAL(rc, 0);
    CU_ASSERT_EQUAL(thread_info.suppressed_records, 0);
    CU_ASSERT_EQUAL(thread_info.interesting_records, 1);

    n_records = process_log_rec_record_count();
    CU_ASSERT_EQUAL(n_records, 1);

    MemFree(rec);

#undef TEST_SHORT_NAME
}

void changelog_test_init(void)
{
    log_config.debug_level = 0;
    memset(&thread_info, 0, sizeof(thread_info));
    rh_list_init(&thread_info.op_queue);
    thread_info.id_hash = id_hash_init(ID_CHGLOG_HASH_SIZE, false);
}

void changelog_test_fini(void)
{
    process_log_rec_queue_cleanup();
    MemFree(thread_info.id_hash);
    memset(&thread_info, 0, sizeof(thread_info));
}

int changelog_suite_init(void);
int changelog_suite_init(void)
{
    cl_reader_config.mdt_count = 1;
    cl_reader_config.mdt_def = MemCalloc(1, sizeof(mdt_def_t));
    if (cl_reader_config.mdt_def == NULL)
        return ENOMEM;

    strcpy(cl_reader_config.mdt_def[0].mdt_name, "fake_mdt");

    return 0;
}

int changelog_suite_fini(void);
int changelog_suite_fini(void)
{
    MemFree(cl_reader_config.mdt_def);
    cl_reader_config.mdt_count = 0;

    module_unload_all();

    return 0;
}

CU_TestInfo changelog_suite[] = {
    UNIT_TEST_INFO(process_log_rec_invalid_test),
    UNIT_TEST_INFO(process_log_rec_ignore_test),
#if defined(HAVE_CHANGELOG_EXTEND_REC) || defined(HAVE_FLEX_CL)
    UNIT_TEST_INFO(process_log_rec_rename_one_zero_tfid_test),
    UNIT_TEST_INFO(process_log_rec_rename_non_zero_tfid_test),
#endif
    UNIT_TEST_INFO(process_log_rec_rename_ext_rec_one_zero_tfid_test),
    UNIT_TEST_INFO(process_log_rec_rename_ext_rec_non_zero_tfid_test),
    UNIT_TEST_INFO(process_log_rec_unlink),
    CU_TEST_INFO_NULL
};
