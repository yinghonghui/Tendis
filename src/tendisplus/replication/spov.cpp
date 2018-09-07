#include <list>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <string>
#include <set>
#include <map>
#include <limits>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/error/en.h"
#include "glog/logging.h"
#include "tendisplus/replication/repl_manager.h"
#include "tendisplus/storage/record.h"
#include "tendisplus/commands/command.h"
#include "tendisplus/utils/scopeguard.h"
#include "tendisplus/utils/redis_port.h"
#include "tendisplus/utils/invariant.h"

namespace tendisplus {

void ReplManager::slaveStartFullsync(const StoreMeta& metaSnapshot) {
    LOG(INFO) << "store:" << metaSnapshot.id << " fullsync start";

    // 1) stop store and clean it's directory
    PStore store = _svr->getSegmentMgr()->getInstanceById(metaSnapshot.id);
    INVARIANT(store != nullptr);

    Status stopStatus = store->stop();
    if (!stopStatus.ok()) {
        // there may be uncanceled transactions binding with the store
        LOG(WARNING) << "stop store:" << metaSnapshot.id
                    << " failed:" << stopStatus.toString();
        return;
    }
    INVARIANT(!store->isRunning());
    Status clearStatus =  store->clear();
    if (!clearStatus.ok()) {
        LOG(FATAL) << "Unexpected store:" << metaSnapshot.id << " clear"
            << " failed:" << clearStatus.toString();
    }

    // 2) require a sync-client
    std::shared_ptr<BlockingTcpClient> client =
        std::move(createClient(metaSnapshot));
    if (client == nullptr) {
        LOG(WARNING) << "startFullSync with: "
                    << metaSnapshot.syncFromHost << ":"
                    << metaSnapshot.syncFromPort
                    << " failed, no valid client";
        return;
    }

    // 3) necessary pre-conditions all ok, startup a guard to rollback
    // state if failed
    bool rollback = true;
    auto guard = MakeGuard([this, &rollback, &metaSnapshot]{
        std::lock_guard<std::mutex> lk(_mutex);
        if (rollback) {
            auto newMeta = metaSnapshot.copy();
            newMeta->replState = ReplState::REPL_CONNECT;
            newMeta->binlogId = Transaction::MAX_VALID_TXNID+1;
            changeReplStateInLock(*newMeta, false);
        }
    });

    // 4) read backupinfo from master
    std::stringstream ss;
    ss << "FULLSYNC " << metaSnapshot.syncFromId;
    client->writeLine(ss.str(), std::chrono::seconds(1));
    Expected<std::string> s = client->readLine(std::chrono::seconds(3));
    if (!s.ok()) {
        LOG(WARNING) << "fullSync req master error:" << s.status().toString();
        return;
    }

    if (s.value().size() == 0 || s.value()[0] == '-') {
        LOG(INFO) << "fullSync req master failed:" << s.value();
        return;
    }

    auto newMeta = metaSnapshot.copy();
    newMeta->replState = ReplState::REPL_TRANSFER;
    newMeta->binlogId = Transaction::MAX_VALID_TXNID+1;
    changeReplState(*newMeta, false);

    auto expFlist = [&s]() -> Expected<std::map<std::string, size_t>> {
        rapidjson::Document doc;
        doc.Parse(s.value());
        if (doc.HasParseError()) {
            return {ErrorCodes::ERR_NETWORK,
                        rapidjson::GetParseError_En(doc.GetParseError())};
        }
        if (!doc.IsObject()) {
            return {ErrorCodes::ERR_NOTFOUND, "flist not json obj"};
        }
        std::map<std::string, size_t> result;
        for (auto& o : doc.GetObject()) {
            if (!o.value.IsUint64()) {
                return {ErrorCodes::ERR_NOTFOUND, "json value not uint64"};
            }
            result[o.name.GetString()] = o.value.GetUint64();
        }
        return result;
    }();

    // TODO(deyukong): split the transfering-physical-task into many
    // small schedule-unit, each processes one file, or a fixed-size block.
    if (!expFlist.ok()) {
        return;
    }

    auto backupExists = [store]() -> Expected<bool> {
        std::error_code ec;
        bool exists = filesystem::exists(
                        filesystem::path(store->backupDir()), ec);
        if (ec) {
            return {ErrorCodes::ERR_INTERNAL, ec.message()};
        }
        return exists;
    }();
    if (!backupExists.ok() || backupExists.value()) {
        LOG(FATAL) << "store:" << metaSnapshot.id << " backupDir exists";
    }

    const std::map<std::string, size_t>& flist = expFlist.value();

    std::set<std::string> finishedFiles;
    while (true) {
        if (finishedFiles.size() == flist.size()) {
            break;
        }
        Expected<std::string> s = client->readLine(std::chrono::seconds(1));
        if (!s.ok()) {
            return;
        }
        if (finishedFiles.find(s.value()) != finishedFiles.end()) {
            LOG(FATAL) << "BUG: fullsync " << s.value() << " retransfer";
        }
        if (flist.find(s.value()) == flist.end()) {
            LOG(FATAL) << "BUG: fullsync " << s.value() << " invalid file";
        }
        std::string fullFileName = store->backupDir() + "/" + s.value();
        filesystem::path fileDir =
                filesystem::path(fullFileName).remove_filename();
        if (!filesystem::exists(fileDir)) {
            filesystem::create_directories(fileDir);
        }
        auto myfile = std::fstream(fullFileName,
                    std::ios::out|std::ios::binary);
        if (!myfile.is_open()) {
            LOG(ERROR) << "open file:" << fullFileName << " for write failed";
            return;
        }
        size_t remain = flist.at(s.value());
        while (remain) {
            size_t batchSize = std::min(remain, size_t(20ULL*1024*1024));
            remain -= batchSize;
            Expected<std::string> exptData =
                client->read(batchSize, std::chrono::seconds(1));
            if (!exptData.ok()) {
                LOG(ERROR) << "fullsync read bulk data failed:"
                            << exptData.status().toString();
                return;
            }
            myfile.write(exptData.value().c_str(), exptData.value().size());
            if (myfile.bad()) {
                LOG(ERROR) << "write file:" << fullFileName
                            << " failed:" << strerror(errno);
                return;
            }
        }
        LOG(INFO) << "fullsync file:" << fullFileName << " transfer done";
        finishedFiles.insert(s.value());
    }

    client->writeLine("+OK", std::chrono::seconds(1));

    // 5) restart store, change to stready-syncing mode
    Expected<uint64_t> restartStatus = store->restart(true);
    if (!restartStatus.ok()) {
        LOG(FATAL) << "fullSync restart store:" << metaSnapshot.id
            << " failed:" << restartStatus.status().toString();
    }
    
    newMeta = metaSnapshot.copy();
    newMeta->replState = ReplState::REPL_CONNECTED;
    newMeta->binlogId = restartStatus.value();

    // in ReplManager.startup(), a dummy binlog is written. here we should not
    // get an empty binlog set.
    INVARIANT(newMeta->binlogId != Transaction::TXNID_UNINITED);
    changeReplState(*newMeta, true);

    rollback = false;

    LOG(INFO) << "store:" << metaSnapshot.id
                << " fullsync Done, files:" << finishedFiles.size()
                << ", binlogId:" << newMeta->binlogId;
}

void ReplManager::slaveChkSyncStatus(const StoreMeta& metaSnapshot) {
    bool reconn = [this, &metaSnapshot] {
        std::lock_guard<std::mutex> lk(_mutex);
        auto sessionId = _syncStatus[metaSnapshot.id]->sessionId;
        auto lastSyncTime = _syncStatus[metaSnapshot.id]->lastSyncTime;
        if (sessionId == std::numeric_limits<uint64_t>::max()) {
            return true;
        }
        if (lastSyncTime + std::chrono::seconds(10) <= SCLOCK::now()) {
            return true;
        }
        return false;
    }();

    if (!reconn) {
        return;
    }
    LOG(INFO) << "store:" << metaSnapshot.id
                << " reconn with:" << metaSnapshot.syncFromHost
                << "," << metaSnapshot.syncFromPort
                << "," << metaSnapshot.syncFromId;

    std::shared_ptr<BlockingTcpClient> client =
        std::move(createClient(metaSnapshot));
    if (client == nullptr) {
        LOG(WARNING) << "store:" << metaSnapshot.id << " reconn master failed";
        return;
    }

    std::stringstream ss;
    ss << "INCRSYNC " << metaSnapshot.syncFromId
        << ' ' << metaSnapshot.id
        << ' ' << metaSnapshot.binlogId;
    client->writeLine(ss.str(), std::chrono::seconds(1));
    Expected<std::string> s = client->readLine(std::chrono::seconds(3));
    if (!s.ok()) {
        LOG(WARNING) << "store:" << metaSnapshot.id
                << " psync master failed with error:" << s.status().toString();
        return;
    }
    if (s.value().size() == 0 || s.value()[0] != '+') {
        LOG(WARNING) << "store:" << metaSnapshot.id
                << " incrsync master bad return:" << s.value();
        return;
    }

    Status pongStatus  = client->writeLine("+PONG", std::chrono::seconds(1));
    if (!pongStatus.ok()) {
        LOG(WARNING) << "store:" << metaSnapshot.id
                << " write pong failed:" << pongStatus.toString();
        return;
    }

    NetworkAsio *network = _svr->getNetwork();
    INVARIANT(network != nullptr);

    // why dare we transfer a client to a session ?
    // 1) the logic gets here, so there wont be any
    // async handlers in the event queue.
    // 2) every handler is trigger by calling client's
    // some read/write/connect functions.
    // 3) master side will read +PONG before sending
    // new data, so there wont be any sticky packets.
    Expected<uint64_t> expSessionId =
            network->client2Session(std::move(client));
    if (!expSessionId.ok()) {
        LOG(WARNING) << "client2Session failed:"
                    << expSessionId.status().toString();
        return;
    }
    uint64_t sessionId = expSessionId.value();

    {
        std::lock_guard<std::mutex> lk(_mutex);
        _syncStatus[metaSnapshot.id]->sessionId = sessionId;
        _syncStatus[metaSnapshot.id]->lastSyncTime = SCLOCK::now();
    }
    LOG(INFO) << "store:" << metaSnapshot.id << " psync master succ";
}

void ReplManager::slaveSyncRoutine(uint32_t storeId) {
    SCLOCK::time_point nextSched = SCLOCK::now();
    auto guard = MakeGuard([this, &nextSched, storeId] {
        std::lock_guard<std::mutex> lk(_mutex);
        INVARIANT(_syncStatus[storeId]->isRunning);
        _syncStatus[storeId]->isRunning = false;
        _syncStatus[storeId]->nextSchedTime = nextSched;
        _cv.notify_all();
    });

    std::unique_ptr<StoreMeta> metaSnapshot = [this, storeId]() {
        std::lock_guard<std::mutex> lk(_mutex);
        return std::move(_syncMeta[storeId]->copy());
    }();

    if (metaSnapshot->syncFromHost == "") {
        // if master is nil, try sched after 1 second
        nextSched = nextSched + std::chrono::seconds(1);
        return;
    }

    INVARIANT(metaSnapshot->replState == ReplState::REPL_CONNECT ||
        metaSnapshot->replState == ReplState::REPL_CONNECTED);

    if (metaSnapshot->replState == ReplState::REPL_CONNECT) {
        slaveStartFullsync(*metaSnapshot);
        nextSched = nextSched + std::chrono::seconds(3);
        return;
    } else if (metaSnapshot->replState == ReplState::REPL_CONNECTED) {
        slaveChkSyncStatus(*metaSnapshot);
        nextSched = nextSched + std::chrono::seconds(10);
        return;
    } else {
        INVARIANT(false);
    }
}

Status ReplManager::applyBinlogs(uint32_t storeId, uint64_t sessionId,
            const std::map<uint64_t, std::list<ReplLog>>& binlogs) {
    [this, storeId]() {
        std::unique_lock<std::mutex> lk(_mutex);
        _cv.wait(lk,
                [this, storeId]
                {return !_syncStatus[storeId]->isRunning;});
        _syncStatus[storeId]->isRunning = true;
    }();

    auto guard = MakeGuard([this, storeId] {
        std::unique_lock<std::mutex> lk(_mutex);
        INVARIANT(_syncStatus[storeId]->isRunning);
        _syncStatus[storeId]->isRunning = false;
        _syncStatus[storeId]->lastSyncTime = SCLOCK::now();
    });

    bool idMatch = [this, storeId, sessionId]() {
        std::unique_lock<std::mutex> lk(_mutex);
        return (sessionId == _syncStatus[storeId]->sessionId);
    }();
    if (!idMatch) {
        return {ErrorCodes::ERR_NOTFOUND, "sessionId not match"};
    }

    for (const auto& logList : binlogs) {
        Status s = applySingleTxn(storeId, logList.first, logList.second);
        if (!s.ok()) {
            return s;
        }
    }

    // TODO(deyukong): perf and maybe periodly save binlogpos
    if (binlogs.size() > 0) {
        std::lock_guard<std::mutex> lk(_mutex);
        auto newMeta = _syncMeta[storeId]->copy();
        newMeta->binlogId = binlogs.rbegin()->first;
        INVARIANT(newMeta->replState == ReplState::REPL_CONNECTED);
        changeReplStateInLock(*newMeta, true);
    }
    return {ErrorCodes::ERR_OK, ""};
}

Status ReplManager::applySingleTxn(uint32_t storeId, uint64_t txnId,
                                   const std::list<ReplLog>& ops) {
    PStore store = _svr->getSegmentMgr()->getInstanceById(storeId);
    INVARIANT(store != nullptr); 
    auto ptxn = store->createTransaction();
    if (!ptxn.ok()) {
        return ptxn.status();
    }

    std::unique_ptr<Transaction> txn = std::move(ptxn.value());
    for (const auto& log : ops) {
        const ReplLogValue& logVal = log.getReplLogValue();

        Expected<RecordKey> expRk = RecordKey::decode(logVal.getOpKey());
        if (!expRk.ok()) {
            return expRk.status();
        }

        auto strPair = log.encode();
        // write binlog
        auto s = store->setKV(strPair.first, strPair.second,
                              txn.get(), false /*withlog*/);
        if (!s.ok()) {
            return s;
        }
        switch (logVal.getOp()) {
            case (ReplOp::REPL_OP_SET): {
                Expected<RecordValue> expRv =
                    RecordValue::decode(logVal.getOpValue());
                if (!expRv.ok()) {
                    return expRv.status();
                }
                s = store->setKV(expRk.value(), expRv.value(),
                                      txn.get(), false /*withlog*/);
                if (!s.ok()) {
                    return s;
                } else {
                    break;
                }
            }
            case (ReplOp::REPL_OP_DEL): {
                s = store->delKV(expRk.value(), txn.get(), false /*withlog*/);
                if (!s.ok()) {
                    return s;
                } else {
                    break;
                }
            }
            default: {
                LOG(FATAL) << "invalid binlogOp:"
                            << static_cast<uint8_t>(logVal.getOp());
            }
        }
    }
    Expected<uint64_t> expCmit = txn->commit();
    if (!expCmit.ok()) {
        return expCmit.status();
    }
    return {ErrorCodes::ERR_OK, ""};
}

}  // namespace tendisplus