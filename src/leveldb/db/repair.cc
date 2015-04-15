// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// We recover the contents of the descriptor from the other files we find.
// (1) Any log files are first converted to tables
// (2) We scan every table to compute
//     (a) smallest/largest for the table
//     (b) largest sequence number in the table
// (3) We generate descriptor contents:
//      - log number is set to zero
//      - next-file-number is set to 1 + largest file number we found
//      - last-sequence-number is set to largest sequence# found across
//        all tables (see 2c)
//      - compaction pointers are cleared
//      - every table file is added at level 0
//
// Possible optimization 1:
//   (a) Compute total size and use to pick appropriate max-level M
//   (b) Sort tables by largest sequence# in the table
//   (c) For each table: if it overlaps earlier table, place in level-0,
//       else place in level-M.
// Possible optimization 2:
//   Store per-table metadata (smallest, largest, largest-seq#, ...)
//   in the table's meta section to speed up ScanTable.

#include <iostream>

#include "db/builder.h"
#include "db/db_impl.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "db/write_batch_internal.h"
#include "leveldb/comparator.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "util/string_ext.h"

namespace leveldb {

namespace {

class DBRepairer;

class Repairer {
 public:
  Repairer(const std::string& dbname, const Options& options)
      : dbname_(dbname),
        env_(options.env),
        icmp_(options.comparator),
        ipolicy_(options.filter_policy),
        options_(SanitizeOptions(dbname, &icmp_, &ipolicy_, options)),
        owns_info_log_(options_.info_log != options.info_log),
        owns_block_cache_(options_.block_cache != options.block_cache),
        owns_table_cache_(options_.table_cache == NULL),
        table_cache_(options_.table_cache),
        next_file_number_(1), mem_(NULL), max_sequence_(0) {
    // TableCache can be small since we expect each table to be opened once.
    if (owns_table_cache_) {
      Log(options_.info_log, "[%s] create new table cache in repairer.", dbname_.c_str());
      table_cache_ = new TableCache(100);
    }
  }

  ~Repairer() {
    if (owns_info_log_) {
      delete options_.info_log;
    }
    if (owns_block_cache_) {
      delete options_.block_cache;
    }
    if (owns_table_cache_) {
      delete table_cache_;
    }
  }

  Status Run() {
    Status status = FindFiles();
    if (status.ok()) {
      ConvertLogFilesToTables();
      ExtractMetaData();
      status = WriteDescriptor();
    }
    if (status.ok()) {
      unsigned long long bytes = 0;
      for (size_t i = 0; i < tables_.size(); i++) {
        bytes += tables_[i].meta.file_size;
      }
      Log(options_.info_log,
          "**** Repaired leveldb %s; "
          "recovered %d files; %llu bytes. "
          "Some data may have been lost. "
          "****",
          dbname_.c_str(),
          static_cast<int>(tables_.size()),
          bytes);
    }
    return status;
  }

 private:
  friend class DBRepairer;
  struct TableInfo {
    FileMetaData meta;
    SequenceNumber max_sequence;
  };

  std::string const dbname_;
  Env* const env_;
  InternalKeyComparator const icmp_;
  InternalFilterPolicy const ipolicy_;
  Options const options_;
  bool owns_info_log_;
  bool owns_block_cache_;
  bool owns_table_cache_;
  TableCache* table_cache_;
  VersionEdit edit_;

  std::vector<std::string> manifests_;
  std::vector<uint64_t> table_numbers_;
  std::vector<uint64_t> logs_;
  std::vector<TableInfo> tables_;
  uint64_t next_file_number_;
  MemTable* mem_;
  uint64_t max_sequence_;

  Status FindFiles() {
    std::vector<std::string> filenames;
    Status status = env_->GetChildren(dbname_, &filenames);
    if (!status.ok()) {
      return status;
    }
    if (filenames.empty()) {
      return Status::IOError(dbname_, "repair found no files");
    }

    uint64_t number;
    FileType type;
    for (size_t i = 0; i < filenames.size(); i++) {
      if (ParseFileName(filenames[i], &number, &type)) {
        if (type == kDescriptorFile) {
          manifests_.push_back(filenames[i]);
        } else {
          if (number + 1 > next_file_number_) {
            next_file_number_ = number + 1;
          }
          if (type == kLogFile) {
            logs_.push_back(number);
          } else if (type == kTableFile) {
            table_numbers_.push_back(BuildFullFileNumber(dbname_, number));
          } else {
            // Ignore other files
          }
        }
      }
    }
    return status;
  }

  void ConvertLogFilesToTables() {
    for (size_t i = 0; i < logs_.size(); i++) {
      std::string logname = LogFileName(dbname_, logs_[i]);
      Status status = ConvertLogToTable(logs_[i]);
      if (!status.ok()) {
        Log(options_.info_log, "[%s] Log #%llu: ignoring conversion error: %s",
            dbname_.c_str(),
            (unsigned long long) logs_[i],
            status.ToString().c_str());
      }
      ArchiveFile(logname);
    }
  }

  Status ConvertLogToTable(uint64_t log) {
    struct LogReporter : public log::Reader::Reporter {
      Env* env;
      Logger* info_log;
      uint64_t lognum;
      virtual void Corruption(size_t bytes, const Status& s) {
        // We print error messages for corruption, but continue repairing.
        Log(info_log, "Log #%llu: dropping %d bytes; %s",
            (unsigned long long) lognum,
            static_cast<int>(bytes),
            s.ToString().c_str());
      }
    };

    // Open the log file
    std::string logname = LogFileName(dbname_, log);
    SequentialFile* lfile;
    Status status = env_->NewSequentialFile(logname, &lfile);
    if (!status.ok()) {
      return status;
    }

    // Create the log reader.
    LogReporter reporter;
    reporter.env = env_;
    reporter.info_log = options_.info_log;
    reporter.lognum = log;
    // We intentially make log::Reader do checksumming so that
    // corruptions cause entire commits to be skipped instead of
    // propagating bad information (like overly large sequence
    // numbers).
    log::Reader reader(lfile, &reporter, false/*do not checksum*/,
                       0/*initial_offset*/);

    // Read all the records and add to a memtable
    std::string scratch;
    Slice record;
    WriteBatch batch;
    mem_ = new MemTable(icmp_);
    mem_->Ref();
    int counter = 0;
    while (reader.ReadRecord(&record, &scratch)) {
      if (record.size() < 12) {
        reporter.Corruption(
            record.size(), Status::Corruption("log record too small"));
        continue;
      }
      WriteBatchInternal::SetContents(&batch, record);
      status = WriteBatchInternal::InsertInto(&batch, mem_);
      if (status.ok()) {
        counter += WriteBatchInternal::Count(&batch);
      } else {
        Log(options_.info_log, "[%s] Log #%llu: ignoring %s",
            dbname_.c_str(),
            (unsigned long long) log,
            status.ToString().c_str());
        status = Status::OK();  // Keep going with rest of file
      }
    }
    delete lfile;

    // Do not record a version edit for this conversion to a Table
    // since ExtractMetaData() will also generate edits.
    FileMetaData meta;
    meta.number = next_file_number_++;
    Iterator* iter = mem_->NewIterator();
    uint64_t saved_bytes = 0;
    status = BuildTable(dbname_, env_, options_, table_cache_,
                        iter, &meta, &saved_bytes);
    delete iter;
    mem_->Unref();
    mem_ = NULL;
    if (status.ok()) {
      if (meta.file_size > 0) {
        table_numbers_.push_back(meta.number);
      }
    }
    Log(options_.info_log, "[%s] Log #%llu: %d ops saved to Table #%llu %s",
        dbname_.c_str(),
        (unsigned long long) log,
        counter,
        (unsigned long long) meta.number,
        status.ToString().c_str());
    return status;
  }

    Status InsertMemTable(WriteBatch* batch, uint64_t batch_seq) {
        if (mem_ == NULL) {
            mem_ = new MemTable(icmp_);
            mem_->Ref();
        }
        assert(batch_seq > max_sequence_);
        max_sequence_ = batch_seq + WriteBatchInternal::Count(batch) - 1;
        return WriteBatchInternal::InsertInto(batch, mem_);
    }
    bool HasMemTable() const {
        return mem_ != NULL;
    }
    Status BuildTableFile(uint64_t log, uint32_t lg_id, uint64_t* file_number) {
        FileMetaData meta;
        meta.number = next_file_number_++;
        *file_number = meta.number;
        Iterator* iter = mem_->NewIterator();
        uint64_t saved_bytes = 0;
        Status status = BuildTable(dbname_, env_, options_, table_cache_,
                                   iter, &meta, &saved_bytes);
        delete iter;
        mem_->Unref();
        mem_ = NULL;
        if (status.ok()) {
            if (meta.file_size > 0) {
                table_numbers_.push_back(meta.number);
            }
        }
        Log(options_.info_log, "[%s][lg:%d] Log #%llu: saved to Table #%llu %s",
            dbname_.c_str(), lg_id,
            (unsigned long long) log,
            (unsigned long long) meta.number,
            status.ToString().c_str());
        return status;
    }

    Status AddTableMeta(uint64_t table_number) {
        TableInfo t;
        t.meta.number = table_number;
        Status status = ScanTable(&t);
        if (!status.ok()) {
            std::string fname = TableFileName(dbname_, table_number);
            Log(options_.info_log, "[%s] Table #%llu: ignoring %s",
                dbname_.c_str(),
                (unsigned long long) table_number,
                status.ToString().c_str());
            ArchiveFile(fname);
        } else {
            tables_.push_back(t);
            table_numbers_.push_back(table_number);
        }
        return status;
    }

  void ExtractMetaData() {
    std::vector<TableInfo> kept;
    for (size_t i = 0; i < table_numbers_.size(); i++) {
      TableInfo t;
      t.meta.number = table_numbers_[i];
      Status status = ScanTable(&t);
      if (!status.ok()) {
        std::string fname = TableFileName(dbname_, table_numbers_[i]);
        Log(options_.info_log, "[%s] Table #%llu: ignoring %s",
            dbname_.c_str(),
            (unsigned long long) table_numbers_[i],
            status.ToString().c_str());
        ArchiveFile(fname);
      } else {
        tables_.push_back(t);
        if (t.max_sequence > max_sequence_) {
            max_sequence_ = t.max_sequence;
        }
      }
    }
  }

  Status ScanTable(TableInfo* t) {
    std::string fname = TableFileName(dbname_, t->meta.number);
    int counter = 0;
    Status status = env_->GetFileSize(fname, &t->meta.file_size);
    if (status.ok()) {
      Iterator* iter = table_cache_->NewIterator(
          ReadOptions(&options_), dbname_, t->meta.number, t->meta.file_size);
      bool empty = true;
      ParsedInternalKey parsed;
      t->max_sequence = 0;
      for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
        Slice key = iter->key();
        if (!ParseInternalKey(key, &parsed)) {
          Log(options_.info_log, "[%s] Table #%llu: unparsable key %s",
              dbname_.c_str(),
              (unsigned long long) t->meta.number,
              EscapeString(key).c_str());
          continue;
        }

        counter++;
        if (empty) {
          empty = false;
          t->meta.smallest.DecodeFrom(key);
        }
        t->meta.largest.DecodeFrom(key);
        if (parsed.sequence > t->max_sequence) {
          t->max_sequence = parsed.sequence;
        }
      }
      if (!iter->status().ok()) {
        status = iter->status();
      }
      delete iter;
      if (status.ok() && empty) {
        status = Status::Corruption("sst is empty");
      }
    }
    Log(options_.info_log, "[%s] Table #%llu: %d entries %s",
        dbname_.c_str(),
        (unsigned long long) t->meta.number,
        counter,
        status.ToString().c_str());
    return status;
  }

  Status WriteDescriptor() {
    std::string tmp = TempFileName(dbname_, 1);
    WritableFile* file;
    Status status = env_->NewWritableFile(tmp, &file);
    if (!status.ok()) {
      return status;
    }

    SequenceNumber max_sequence = 0;
    for (size_t i = 0; i < tables_.size(); i++) {
      if (max_sequence < tables_[i].max_sequence) {
        max_sequence = tables_[i].max_sequence;
      }
    }

    edit_.SetComparatorName(icmp_.user_comparator()->Name());
    edit_.SetLogNumber(0);
    edit_.SetNextFile(next_file_number_);
    edit_.SetLastSequence(max_sequence);

    for (size_t i = 0; i < tables_.size(); i++) {
      // TODO(opt): separate out into multiple levels
      const TableInfo& t = tables_[i];
      edit_.AddFile(0, t.meta.number, t.meta.file_size,
                    t.meta.smallest, t.meta.largest);
    }

    //fprintf(stderr, "NewDescriptor:\n%s\n", edit_.DebugString().c_str());
    {
      log::Writer log(file);
      std::string record;
      edit_.EncodeTo(&record);
      status = log.AddRecord(record);
    }
    if (status.ok()) {
      status = file->Close();
    }
    delete file;
    file = NULL;

    if (!status.ok()) {
      env_->DeleteFile(tmp);
    } else {
      // Discard older manifests
      for (size_t i = 0; i < manifests_.size(); i++) {
        ArchiveFile(dbname_ + "/" + manifests_[i]);
      }

      // Install new manifest
      status = env_->RenameFile(tmp, DescriptorFileName(dbname_, 1));
      if (status.ok()) {
        status = SetCurrentFile(env_, dbname_, 1);
      } else {
        env_->DeleteFile(tmp);
      }
    }
    return status;
  }

  void ArchiveFile(const std::string& fname) {
    // Move into another directory.  E.g., for
    //    dir/foo
    // rename to
    //    dir/lost/foo
    const char* slash = strrchr(fname.c_str(), '/');
    std::string new_dir;
    if (slash != NULL) {
      new_dir.assign(fname.data(), slash - fname.data());
    }
    new_dir.append("/lost");
    env_->CreateDir(new_dir);  // Ignore error
    std::string new_file = new_dir;
    new_file.append("/");
    new_file.append((slash == NULL) ? fname.c_str() : slash + 1);
    Status s = env_->RenameFile(fname, new_file);
    Log(options_.info_log, "[%s] Archiving %s: %s\n",
        dbname_.c_str(),
        fname.c_str(), s.ToString().c_str());
  }
};

Options InitDefaultOptions(const Options& options, const std::string& dbname) {
    Options opt = options;

    Status s = opt.env->CreateDir(dbname);
    if (!s.ok()) {
        std::cerr << "[" << dbname << "] fail to create dir: "
            << s.ToString() << std::endl;
    }
    assert(s.ok());

    if (opt.exist_lg_list == NULL) {
        opt.exist_lg_list = new std::set<uint32_t>;
        opt.exist_lg_list->insert(0);
    }
    return opt;
}

class DBRepairer {
public:
    DBRepairer(const std::string& dbname, const Options& options)
        : dbname_(dbname), env_(options.env),
          options_(InitDefaultOptions(options, dbname)),
          created_own_lg_list_(options_.exist_lg_list != options.exist_lg_list),
          log_number_(0),
          last_sequence_(0) {
        std::set<uint32_t>::iterator it = options_.exist_lg_list->begin();
        for (; it != options_.exist_lg_list->end(); ++it) {
            Repairer* repair = new Repairer(dbname_ + "/" + Uint64ToString(*it),
                                            options_);
            repairers.push_back(repair);
        }
    }
    ~DBRepairer() {
        std::set<uint32_t>::iterator it = options_.exist_lg_list->begin();
        for (; it != options_.exist_lg_list->end(); ++it) {
            delete repairers[*it];
        }
        if (created_own_lg_list_) {
            delete options_.exist_lg_list;
        }
    }

    Status Run() {
        Status status = FindFiles();
        if (status.ok()) {
            ExtractMetaData();
            ConvertLogFilesToTables();
            status = WriteDescriptor();
        }
        return status;
    }

private:
    Status FindFiles() {
        std::vector<std::string> filenames;
        Status status = env_->GetChildren(dbname_, &filenames);
        if (!status.ok()) {
            return status;
        }
        if (filenames.empty()) {
            return Status::IOError(dbname_, "repair found no files");
        }

        uint64_t number;
        FileType type;
        for (size_t i = 0; i < filenames.size(); i++) {
            if (ParseFileName(filenames[i], &number, &type)) {
                if (type == kLogFile) {
                    logfiles_.push_back(number);
                    if (number + 1 > log_number_) {
                        log_number_ = number + 1;
                    }
                }
            }
        }

        std::set<uint32_t>::iterator it = options_.exist_lg_list->begin();
        for (; it != options_.exist_lg_list->end(); ++it) {
            repairers[*it]->FindFiles();
        }
        return status;
    }

    void ConvertLogFilesToTables() {
        for (size_t i = 0; i < logfiles_.size(); i++) {
            std::string logname = LogHexFileName(dbname_, logfiles_[i]);
            Status status = ConvertLogToTable(logfiles_[i]);
            if (!status.ok()) {
                Log(options_.info_log, "[%s] Log #%llu: ignoring conversion error: %s",
                    dbname_.c_str(),
                    (unsigned long long) logfiles_[i],
                    status.ToString().c_str());
            }
            ArchiveFile(logname);
        }
    }

    Status ConvertLogToTable(uint64_t log) {
        struct LogReporter : public log::Reader::Reporter {
            Env* env;
            Logger* info_log;
            uint64_t lognum;
            virtual void Corruption(size_t bytes, const Status& s) {
            // We print error messages for corruption, but continue repairing.
            Log(info_log, "Log #%llu: dropping %d bytes; %s",
                (unsigned long long) lognum,
                static_cast<int>(bytes),
                s.ToString().c_str());
            }
        };

        // Open the log file
        std::string logname = LogHexFileName(dbname_, log);
        SequentialFile* lfile;
        Status status = env_->NewSequentialFile(logname, &lfile);
        if (!status.ok()) {
            return status;
        }

        // Create the log reader.
        LogReporter reporter;
        reporter.env = env_;
        reporter.info_log = options_.info_log;
        reporter.lognum = log;

        log::Reader reader(lfile, &reporter, false/*do not checksum*/,
                           0/*initial_offset*/);

        // Read all the records and add to a memtable
        std::string scratch;
        Slice record;
        WriteBatch batch;
        int32_t counter = 0;
        while (reader.ReadRecord(&record, &scratch)) {
            if (record.size() < 12) {
                reporter.Corruption(
                    record.size(), Status::Corruption("log record too small"));
                continue;
            }
            WriteBatchInternal::SetContents(&batch, record);
            uint64_t batch_seq = WriteBatchInternal::Sequence(&batch);
            uint64_t batch_count = WriteBatchInternal::Count(&batch);
            if (batch_seq <= last_sequence_) {
                Log(options_.info_log, "[%s] duplicate record, ignore %llu ~ %llu",
                    dbname_.c_str(), static_cast<unsigned long long>(batch_seq),
                    static_cast<unsigned long long>(batch_seq + batch_count - 1));
                continue;
            }

            std::vector<WriteBatch*> lg_batchs;
            lg_batchs.resize(options_.exist_lg_list->size());
            std::fill(lg_batchs.begin(), lg_batchs.end(), (WriteBatch*)0);
            bool created_new_wb = false;
            if (options_.exist_lg_list->size() > 1) {
                status = batch.SeperateLocalityGroup(&lg_batchs);
                created_new_wb = true;
                if (!status.ok()) {
                    return status;
                }
                for (uint32_t i = 0; i < options_.exist_lg_list->size(); ++i) {
                    if (lg_batchs[i] != 0) {
                        WriteBatchInternal::SetSequence(lg_batchs[i], batch_seq);
                    }
                }
            } else {
                lg_batchs[0] = (&batch);
            }
            for (uint32_t i = 0; i < lg_batchs.size(); ++i) {
                if (lg_batchs[i] == NULL) {
                    continue;
                }
                status = repairers[i]->InsertMemTable(lg_batchs[i], batch_seq);
                if (!status.ok()) {
                    Log(options_.info_log, "[%s][lg:%d] Insert log #%llu: ignoring %s",
                        dbname_.c_str(), i,
                        (unsigned long long) log,
                        status.ToString().c_str());
                    status = Status::OK();  // Keep going with rest of file
                } else {
                    counter += WriteBatchInternal::Count(lg_batchs[i]);
                }
            }
            if (created_new_wb) {
                for (uint32_t i = 0; i < lg_batchs.size(); ++i) {
                    if (lg_batchs[i] != NULL) {
                        delete lg_batchs[i];
                        lg_batchs[i] = NULL;
                    }
                }
            }
            last_sequence_ = batch_seq + batch_count - 1;
        }
        delete lfile;

        std::set<uint32_t>::iterator it = options_.exist_lg_list->begin();
        for (; it != options_.exist_lg_list->end(); ++it) {
            uint32_t i = *it;
            uint64_t file_num = 0;
            if (!repairers[i]->HasMemTable()) {
                continue;
            }
            status = repairers[i]->BuildTableFile(log, i, &file_num);
            if (!status.ok()) {
                Log(options_.info_log, "[%s][lg:%d] BuildLogFile #%llu: ignoring %s",
                    dbname_.c_str(), i,
                    (unsigned long long) log,
                    status.ToString().c_str());
                status = Status::OK();  // Keep going with rest of file
            } else {
                status = repairers[i]->AddTableMeta(file_num);
                if (!status.ok()) {
                    Log(options_.info_log, "[%s][lg:%d] AddTableMeta #%llu: ignoring %s",
                        dbname_.c_str(), i,
                        (unsigned long long) log,
                        status.ToString().c_str());
                    status = Status::OK();  // Keep going with rest of file
                }
            }
        }
        Log(options_.info_log, "[%s] Log #%llu to Table: %d entries %s",
            dbname_.c_str(),
            (unsigned long long) log,
            counter,
            status.ToString().c_str());
        return status;
    }

    void ExtractMetaData() {
        std::set<uint32_t>::iterator it = options_.exist_lg_list->begin();
        for (; it != options_.exist_lg_list->end(); ++it) {
            repairers[*it]->ExtractMetaData();
            if (last_sequence_ < repairers[*it]->max_sequence_) {
                last_sequence_ = repairers[*it]->max_sequence_;
            }
        }
    }

    Status WriteDescriptor() {
        Status status;
        std::set<uint32_t>::iterator it = options_.exist_lg_list->begin();
        for (; it != options_.exist_lg_list->end(); ++it) {
            Status s = repairers[*it]->WriteDescriptor();
            if (!s.ok()) {
                Log(options_.info_log, "[%s][lg:%d] WriteDescriptor error: %s",
                    dbname_.c_str(), *it,
                    s.ToString().c_str());
                status = s;
            }
        }
        return status;
    }

    void ArchiveFile(const std::string& fname) {
        repairers[0]->ArchiveFile(fname);
    }

private:
    std::vector<Repairer*> repairers;
    std::string const dbname_;
    Env* const env_;
    Options const options_;
    bool created_own_lg_list_;
    uint64_t log_number_;
    std::vector<uint64_t> logfiles_;
    uint64_t last_sequence_;
};

}  // namespace

Status RepairDB(const std::string& dbname, const Options& options) {
  DBRepairer repairer(dbname, options);
  return repairer.Run();
}

}  // namespace leveldb
