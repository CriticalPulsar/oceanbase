/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef OCEANBASE_LIBOBLOG_INSTANCE_H__
#define OCEANBASE_LIBOBLOG_INSTANCE_H__

#include "liboblog.h"

#include "lib/allocator/ob_concurrent_fifo_allocator.h"   // ObConcurrentFIFOAllocator
#include "lib/alloc/memory_dump.h"                        // memory_meta_dump

#include "ob_log_binlog_record.h"                         // ObLogBR
#include "ob_obj2str_helper.h"                            // ObObj2strHelper
#include "ob_log_task_pool.h"                             // ObLogTransTaskPool
#include "ob_log_entry_task_pool.h"                       // ObLogEntryTaskPool
#include "ob_log_binlog_record_queue.h"                   // BRQueue
#include "ob_log_trans_stat_mgr.h"                        // TransTpsRpsStatInfo, IObLogTransStatMgr
#include "ob_log_systable_helper.h"                       // ObLogSysTableHelper
#include "ob_log_hbase_mode.h"                            // ObLogHbaseUtil
#include "ob_log_mysql_proxy.h"                           // ObLogMysqlProxy
#include "ob_log_work_mode.h"                             // WorkingMode

using namespace oceanbase::logmessage;

namespace oceanbase
{

namespace common
{
namespace sqlclient
{
class ObMySQLServerProvider;
} // namespace sqlclient
} // namespace common

namespace liboblog
{
class IObLogMetaManager;
class IObLogSchemaGetter;
class IObLogTimeZoneInfoGetter;
class IObLogFetcher;
class IObLogDDLHandler;
class IObLogDmlParser;
class IObLogDdlParser;
class IObLogPartTransParser;
class IObLogSequencer;
class IObLogFormatter;
class IObLogStorager;
class IObLogDataProcessor;
class IObLogCommitter;
class PartTransTask;
class IObLogStartSchemaMatcher;
class IObLogTableMatcher;
class IObLogTransCtxMgr;
class IObStoreService;
class IObLogBRPool;
class IObLogResourceCollector;
class IObLogTenantMgr;
class ObLogTenantGuard;

typedef ObLogTransTaskPool<PartTransTask> PartTransTaskPool;

// interface for error handler
class IObLogErrHandler
{
public:
  virtual ~IObLogErrHandler() {}

public:
  virtual void handle_error(const int err_no, const char *fmt, ...) = 0;
};

typedef common::sqlclient::ObMySQLServerProvider ServerProviderType;

class ObLogInstance : public IObLog, public IObLogErrHandler
{
public:
  virtual ~ObLogInstance();

  static const int64_t TASK_POOL_ALLOCATOR_PAGE_SIZE = common::OB_MALLOC_BIG_BLOCK_SIZE;
  static const int64_t TASK_POOL_ALLOCATOR_TOTAL_LIMIT = (1LL << 37); // 128G
  static const int64_t TASK_POOL_ALLOCATOR_HOLD_LIMIT = TASK_POOL_ALLOCATOR_PAGE_SIZE;
  static const int64_t DATA_OP_TIMEOUT = 10L * 1000L * 1000L;
  static const int64_t CLEAN_LOG_INTERVAL = 60L * 1000L * 1000L;
  static const int64_t REFRESH_SERVER_LIST_INTERVAL = 10L * 1000L * 1000L;

protected:
  ObLogInstance();

public:
  static ObLogInstance *get_instance();
  static ObLogInstance &get_ref_instance();
  static void destroy_instance();

public:
  virtual int init(const char *config_file,
      const uint64_t start_timestamp,
      ERROR_CALLBACK err_cb = NULL);

  // Start-up timestamps are in microseconds
  int init_with_start_tstamp_usec(const char *config_file,
      const uint64_t start_timestamp_usec,
      ERROR_CALLBACK err_cb = NULL);

  virtual int init(const std::map<std::string, std::string>& configs,
      const uint64_t start_timestamp,
      ERROR_CALLBACK err_cb = NULL);

  virtual int init_with_start_tstamp_usec(const std::map<std::string, std::string>& configs,
      const uint64_t start_timestamp_usec,
      ERROR_CALLBACK err_cb = NULL);

  virtual void destroy();

  virtual int next_record(ILogRecord **record, const int64_t timeout_us);
  virtual int next_record(ILogRecord **record,
      int32_t &major_version,
      uint64_t &tenant_id,
      const int64_t timeout_us);
  virtual void release_record(ILogRecord *record);
  virtual int launch();
  virtual void stop();
  virtual int table_group_match(const char *pattern,
      bool &is_matched,
      const int fnmatch_flags = FNM_CASEFOLD);
  virtual int get_table_groups(std::vector<std::string> &table_groups);
  virtual int get_tenant_ids(std::vector<uint64_t> &tenant_ids);

public:
  void mark_stop_flag();
  void handle_error(const int err_no, const char *fmt, ...);
  void timer_routine();
  void sql_thread_routine();
  void flow_control_thread_routine();

public:
  int32_t get_log_level() const;
  const char *get_log_file() const;
  void set_disable_redirect_log(const bool flag) { disable_redirect_log_ = flag; }
  static void print_version();
  int set_assign_log_dir(const char *log_dir, const int64_t log_dir_len);
  void enable_schema_split_mode()
  {
    ATOMIC_STORE(&is_schema_split_mode_, true);
  }

public:
  // Backup using the interface:
  // 1. set ddl schema version
  // For schema non-split mode.
  // Pass in the maximum ddl_schema_version for all backup tenants, sequencer takes the maximum of start_schema_version and that value
  // as the initial value of the global Schema version number, to ensure that the schema version of each tenant's data stream is not rolled back
  // For schema splitting mode, it needs to be set based on tenant_id
  //
  // @retval OB_SUCCESS           Success
  // @retval OB_ENTRY_NOT_EXIST   tenant does not exist, tenant has been deleted
  // @retval Other error codes    Fail
  int set_data_start_ddl_schema_version(const uint64_t tenant_id,
      const int64_t ddl_schema_version);
  // 2. Get the starting schema version by tenant ID
  // No need to check suspension status for new tenants in the middle
  //
  // @retval OB_SUCCESS           Success
  // @retval OB_ENTRY_NOT_EXIST   tenant does not exist, tenant has been deleted
  // @retval Other error codes    Fail
  int get_start_schema_version(const uint64_t tenant_id,
      const bool is_create_tenant_when_backup,
      int64_t &start_schema_version);
  // 3.  set start global trans version
  int set_start_global_trans_version(const int64_t start_global_trans_version);

public:
  friend class ObLogGlobalContext;

private:
  int init_logger_();
  int dump_config_();
  int init_sys_var_for_generate_column_schema_();
  int init_common_(const uint64_t start_timestamp, ERROR_CALLBACK err_cb);
  int get_pid_();
  int init_self_addr_();
  int init_schema_split_mode_(const int64_t sys_schema_version);
  int init_schema_(const int64_t start_tstamp_us, int64_t &sys_start_schema_version);
  int init_components_(const uint64_t start_timestamp);
  int config_tenant_mgr_(const int64_t start_tstamp_us, const int64_t sys_schema_version);
  void destroy_components_();
  void write_pid_file_();
  static void *timer_thread_func_(void *args);
  static void *sql_thread_func_(void *args);
  static void *flow_control_thread_func_(void *args);
  int start_threads_();
  void wait_threads_stop_();
  void reload_config_();
  void print_tenant_memory_usage_();
  void global_flow_control_();
  void dump_pending_trans_info_();
  int revert_participants_(PartTransTask *participants);
  int revert_trans_task_(PartTransTask *task);
  void clean_log_();
  int init_global_tenant_manager_();
  int init_global_kvcache_();
  // Get the total amount of memory occupied by liboblog
  int64_t get_memory_hold_();
  // Get system free memory
  int64_t get_memory_avail_();
  // Get system memory limit
  int64_t get_memory_limit_();
  // Get the number of tasks to be processed
  int get_task_count_(int64_t &ready_to_seq_task_count,
      int64_t &part_trans_task_resuable_count);

  // next record
  void do_drc_consume_tps_stat_();
  void do_drc_consume_rps_stat_();
  // release record
  void do_drc_release_tps_stat_();
  void do_drc_release_rps_stat_();
  // statistical number of tasks
  void do_stat_for_part_trans_task_count_(int record_type,
      int64_t part_trans_task_count,
      bool need_accumulate_stat);
  int64_t get_out_part_trans_task_count_() const { return ATOMIC_LOAD(&part_trans_task_count_); }

  // Print transaction statistics
  void print_trans_stat_();
  // verify ObTraceId
  int init_ob_trace_id_(const char *ob_trace_id_ptr);
  int verify_ob_trace_id_(ILogRecord *record);
  // verify ddl schema version
  int verify_ddl_schema_version_(ILogRecord *br);
  // verify dml unique id
  int verify_dml_unique_id_(ILogRecord *br);
  int get_br_filter_value_(ILogRecord &br,
      const int64_t idx,
      common::ObString &str);
  int query_cluster_info_(ObLogSysTableHelper::ClusterInfo &cluser_info);
  void update_cluster_version_();
  int check_ob_version_legal_(const uint64_t ob_version);
  int check_observer_version_valid_();
  int init_ob_cluster_version_();
  int init_oblog_version_components_();
  void cal_version_components_(const uint64_t version, uint32_t &major, uint16_t &minor, uint16_t &patch);
  int query_cluster_min_observer_version_(uint64_t &min_observer_version);
  // Initialize global variables for compatibility with GCTX dependencies
  void init_global_context_();
  int config_data_start_schema_version_(const int64_t global_data_start_schema_version);
  int update_data_start_schema_version_on_split_mode_();

private:
  static ObLogInstance *instance_;

private:
  bool                    inited_;
  uint32_t                oblog_major_;
  uint16_t                oblog_minor_;
  uint16_t                oblog_patch_;
  pthread_t               timer_tid_;           // Thread that perform light-weight tasks
  pthread_t               sql_tid_;             // Thread that perform SQL-related tasks
  pthread_t               flow_control_tid_;    // Thread that perform flow control
  ERROR_CALLBACK          err_cb_;
  int                     global_errno_;
  int8_t                  handle_error_flag_;
  bool                    disable_redirect_log_;
  int64_t                 log_clean_cycle_time_us_;

  int64_t output_dml_br_count_ CACHE_ALIGNED;
  int64_t output_ddl_br_count_ CACHE_ALIGNED;

  volatile bool           stop_flag_ CACHE_ALIGNED;
  // Record microsecond timestamps
  int64_t                 last_heartbeat_timestamp_micro_sec_ CACHE_ALIGNED;

  // Specify the OB_LOGGER directory path
  char                    assign_log_dir_[common::OB_MAX_FILE_NAME_LENGTH];
  bool                    is_assign_log_dir_valid_;

  // ob_trace_id
  char                    ob_trace_id_str_[common::OB_MAX_TRACE_ID_BUFFER_SIZE + 1];
  uint64_t                br_index_in_trans_;

  // Count the number of partitioned transaction tasks
  // Users holding unreturned
  int64_t                 part_trans_task_count_ CACHE_ALIGNED;

  // Partitioned Task Pool allocator
  common::ObConcurrentFIFOAllocator trans_task_pool_alloc_;

  // External global exposure of variables via TCTX
public:
  int64_t                   start_tstamp_;
  bool                      is_schema_split_mode_;
  std::string               drc_message_factory_binlog_record_type_;
  WorkingMode               working_mode_;

  // compoments
  ObLogMysqlProxy           mysql_proxy_;
  IObLogTimeZoneInfoGetter  *timezone_info_getter_;
  ObLogHbaseUtil            hbase_util_;
  ObObj2strHelper           obj2str_helper_;
  BRQueue                   br_queue_;
  PartTransTaskPool         trans_task_pool_;
  IObLogEntryTaskPool       *log_entry_task_pool_;
  IObStoreService           *store_service_;
  IObLogBRPool              *br_pool_;
  IObLogTransCtxMgr         *trans_ctx_mgr_;
  IObLogMetaManager         *meta_manager_;
  IObLogResourceCollector   *resource_collector_;
  ServerProviderType        *server_provider_;
  IObLogSchemaGetter        *schema_getter_;
  IObLogTableMatcher        *tb_matcher_;
  IObLogStartSchemaMatcher  *ss_matcher_;
  ObLogSysTableHelper       *systable_helper_;
  IObLogCommitter           *committer_;
  IObLogStorager            *storager_;
  IObLogDataProcessor       *data_processor_;
  IObLogFormatter           *formatter_;
  IObLogSequencer           *sequencer_;
  IObLogPartTransParser     *part_trans_parser_;
  IObLogDmlParser           *dml_parser_;
  IObLogDdlParser           *ddl_parser_;
  IObLogDDLHandler          *ddl_handler_;
  IObLogFetcher             *fetcher_;
  IObLogTransStatMgr        *trans_stat_mgr_;                    // Transaction Statistics Management
  IObLogTenantMgr           *tenant_mgr_;
  // The tz information of the sys tenant is placed in instance because of the refresh schema dependency
  ObTZInfoMap               tz_info_map_;
  ObTimeZoneInfoWrap        tz_info_wrap_;

  // Functions exposed to the outside via TCTX
public:
  // @retval OB_SUCCESS             success
  // @retval OB_ENTRY_NOT_EXIST     tenant not exist
  // @retval other error code       fail
  int get_tenant_guard(const uint64_t tenant_id, ObLogTenantGuard &guard);

private:
  DISALLOW_COPY_AND_ASSIGN(ObLogInstance);
};

#define TCTX (ObLogInstance::get_ref_instance())

} // namespace liboblog
} // namespace oceanbase
#endif /* OCEANBASE_LIBOBLOG_INSTANCE_H__ */
