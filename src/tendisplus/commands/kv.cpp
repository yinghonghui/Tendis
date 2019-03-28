#include <string>
#include <utility>
#include <memory>
#include <algorithm>
#include <cctype>
#include <clocale>
#include <vector>
#include "glog/logging.h"
#include "tendisplus/utils/sync_point.h"
#include "tendisplus/utils/string.h"
#include "tendisplus/utils/invariant.h"
#include "tendisplus/utils/redis_port.h"
#include "tendisplus/commands/command.h"

namespace tendisplus {

constexpr int32_t REDIS_SET_NO_FLAGS = 0;

// set if not exists
constexpr int32_t REDIS_SET_NX = (1<<0);

// set if exists
constexpr int32_t REDIS_SET_XX = (1<<1);

// set and expire if not exists
constexpr int32_t REDIS_SET_NXEX = (1<<2);

struct SetParams {
    SetParams()
        :key(""),
         value(""),
         flags(REDIS_SET_NO_FLAGS),
         expire(0) {
    }
    std::string key;
    std::string value;
    int32_t flags;
    uint64_t expire;
};

// TODO(deyukong): unittest of expire
Expected<std::string> setGeneric(PStore store, Transaction *txn,
            int32_t flags, const RecordKey& key, const RecordValue& val,
            const std::string& okReply, const std::string& abortReply) {
    if ((flags & REDIS_SET_NX) || (flags & REDIS_SET_XX)
            || (flags & REDIS_SET_NXEX)) {
        Expected<RecordValue> eValue = store->getKV(key, txn);
        if ((!eValue.ok()) &&
                eValue.status().code() != ErrorCodes::ERR_NOTFOUND) {
            return eValue.status();
        }

        uint64_t currentTs = 0;
        uint64_t targetTtl = 0;
        if (eValue.ok()) {
            currentTs = msSinceEpoch();
            targetTtl = eValue.value().getTtl();
        }
        bool needExpire = (targetTtl != 0
                           && currentTs >= eValue.value().getTtl());
        bool exists =
            (eValue.status().code() == ErrorCodes::ERR_OK) && (!needExpire);
        if ((flags & REDIS_SET_NX && exists) ||
                (flags & REDIS_SET_XX && (!exists)) ||
                (flags & REDIS_SET_NXEX && exists)) {
            // we will early return, we should del the expired key
            // if needed.
            if (needExpire) {
                Status status = store->delKV(key, txn);
                if (!status.ok()) {
                    return status;
                }
                Expected<uint64_t> exptCommit = txn->commit();
                if (!exptCommit.ok()) {
                    return exptCommit.status();
                }
            }
            return abortReply == "" ? Command::fmtNull() : abortReply;
        }
    }

    // here we have no need to check expire since we will overwrite it
    Status status = store->setKV(key, val, txn);
    TEST_SYNC_POINT("setGeneric::SetKV::1");
    if (!status.ok()) {
        return status;
    }
    Expected<uint64_t> exptCommit = txn->commit();
    if (!exptCommit.ok()) {
        return exptCommit.status();
    }
    return okReply == "" ? Command::fmtOK() : okReply;
}

class SetCommand: public Command {
 public:
    SetCommand()
        :Command("set") {
    }

    Expected<SetParams> parse(Session *sess) const {
        const auto& args = sess->getArgs();
        SetParams result;
        if (args.size() < 3) {
            return {ErrorCodes::ERR_PARSEPKT, "invalid set params"};
        }
        result.key = args[1];
        result.value = args[2];
        try {
            for (size_t i = 3; i < args.size(); i++) {
                const std::string& s = toLower(args[i]);
                if (s == "nx") {
                    result.flags |= REDIS_SET_NX;
                } else if (s == "xx") {
                    result.flags |= REDIS_SET_XX;
                } else if (s == "ex" && i+1 < args.size()) {
                    result.expire = std::stoul(args[i+1])*1000ULL;
                    i++;
                } else if (s == "px" && i+1 < args.size()) {
                    result.expire = std::stoul(args[i+1]);
                    i++;
                } else {
                    return {ErrorCodes::ERR_PARSEPKT, "syntax error"};
                }
            }
        } catch (std::exception& ex) {
            LOG(WARNING) << "parse setParams failed:" << ex.what();
            return {ErrorCodes::ERR_PARSEPKT,
                    "value is not an integer or out of range"};
        }
        return result;
    }

    ssize_t arity() const {
        return -3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        Expected<SetParams> exptParams = parse(sess);
        if (!exptParams.ok()) {
            return exptParams.status();
        }

        // NOTE(deyukong): no need to do a expireKeyIfNeeded
        // on a simple kv. We will overwrite it.
        const SetParams& params = exptParams.value();
        auto server = sess->getServerEntry();
        INVARIANT(server != nullptr);
        auto expdb = server->getSegmentMgr()->getDbWithKeyLock(sess, params.key, mgl::LockMode::LOCK_X);
        if (!expdb.ok()) {
            return expdb.status();
        }
        PStore kvstore = expdb.value().store;
        auto ptxn = kvstore->createTransaction();
        if (!ptxn.ok()) {
            return ptxn.status();
        }
        std::unique_ptr<Transaction> txn = std::move(ptxn.value());

        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);

        RecordKey rk(expdb.value().chunkId, pCtx->getDbId(),
                     RecordType::RT_KV, params.key, "");

        uint64_t ts = 0;
        if (params.expire != 0) {
            ts = msSinceEpoch() + params.expire;
        }
        RecordValue rv(params.value, ts);

        for (int32_t i = 0; i < RETRY_CNT - 1; ++i) {
            auto result = setGeneric(kvstore, txn.get(), params.flags,
                                     rk, rv, "", "");
            if (result.status().code() != ErrorCodes::ERR_COMMIT_RETRY) {
                return result;
            }
            ptxn = kvstore->createTransaction();
            if (!ptxn.ok()) {
                return ptxn.status();
            }
            txn = std::move(ptxn.value());
        }
        return setGeneric(kvstore, txn.get(), params.flags,
            rk, rv, "", "");
    }
} setCommand;

class SetexGeneralCommand: public Command {
 public:
    explicit SetexGeneralCommand(const std::string& name)
        :Command(name) {
    }

    ssize_t arity() const {
        return 4;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> runGeneral(Session *sess,
                                     const std::string& key,
                                     const std::string& val,
                                     uint64_t ttl) {
        auto server = sess->getServerEntry();
        INVARIANT(server != nullptr);
        auto expdb = server->getSegmentMgr()->getDbWithKeyLock(sess, key, mgl::LockMode::LOCK_X);
        if (!expdb.ok()) {
            return expdb.status();
        }
        PStore kvstore = expdb.value().store;
        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);

        RecordKey rk(expdb.value().chunkId, pCtx->getDbId(), RecordType::RT_KV, key, "");
        RecordValue rv(val, ttl);
        for (int32_t i = 0; i < RETRY_CNT; ++i) {
            auto ptxn = kvstore->createTransaction();
            if (!ptxn.ok()) {
                return ptxn.status();
            }
            std::unique_ptr<Transaction> txn = std::move(ptxn.value());
            auto result = setGeneric(kvstore,
                                     txn.get(),
                                     REDIS_SET_NO_FLAGS,
                                     rk,
                                     rv,
                                     "",
                                     "");
            if (result.ok()) {
                return result.value();
            }
            if (result.status().code() != ErrorCodes::ERR_COMMIT_RETRY) {
                return result.status();
            }
            if (i == RETRY_CNT - 1) {
                return result.status();
            } else {
                continue;
            }
        }

        INVARIANT(0);
        return {ErrorCodes::ERR_INTERNAL, "not reachable"};
    }
};

class SetExCommand: public SetexGeneralCommand {
 public:
    SetExCommand()
        :SetexGeneralCommand("setex") {
    }

    Expected<std::string> run(Session *sess) final {
        const std::string& key = sess->getArgs()[1];
        const std::string& val = sess->getArgs()[3];
        Expected<uint64_t> eexpire = ::tendisplus::stoul(sess->getArgs()[2]);
        if (!eexpire.ok()) {
            return eexpire.status();
        }
        return runGeneral(sess, key, val,
                          msSinceEpoch() + eexpire.value()*1000);
    }
} setexCmd;

class PSetExCommand: public SetexGeneralCommand {
 public:
    PSetExCommand()
        :SetexGeneralCommand("psetex") {
    }

    Expected<std::string> run(Session *sess) final {
        const std::string& key = sess->getArgs()[1];
        const std::string& val = sess->getArgs()[3];
        Expected<uint64_t> eexpire = ::tendisplus::stoul(sess->getArgs()[2]);
        if (!eexpire.ok()) {
            return eexpire.status();
        }
        return runGeneral(sess, key, val,
                          msSinceEpoch() + eexpire.value());
    }
} psetexCmd;

class SetNxCommand: public Command {
 public:
    SetNxCommand()
        :Command("setnx") {
    }

    ssize_t arity() const {
        return 3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        const std::string& key = sess->getArgs()[1];
        const std::string& val = sess->getArgs()[2];

        auto server = sess->getServerEntry();
        INVARIANT(server != nullptr);
        auto expdb = server->getSegmentMgr()->getDbWithKeyLock(sess, key, mgl::LockMode::LOCK_X);
        if (!expdb.ok()) {
            return expdb.status();
        }
        PStore kvstore = expdb.value().store;
        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);

        RecordKey rk(expdb.value().chunkId, pCtx->getDbId(),
                     RecordType::RT_KV, key, "");
        RecordValue rv(val);
        for (int32_t i = 0; i < RETRY_CNT; ++i) {
            auto ptxn = kvstore->createTransaction();
            if (!ptxn.ok()) {
                return ptxn.status();
            }
            std::unique_ptr<Transaction> txn = std::move(ptxn.value());
            auto result = setGeneric(kvstore,
                                     txn.get(),
                                     REDIS_SET_NX,
                                     rk,
                                     rv,
                                     Command::fmtOne(),
                                     Command::fmtZero());
            if (result.ok()) {
                return result.value();
            }
            if (result.status().code() != ErrorCodes::ERR_COMMIT_RETRY) {
                return result.status();
            }
            if (i == RETRY_CNT - 1) {
                return result.status();
            } else {
                continue;
            }
        }

        INVARIANT(0);
        return {ErrorCodes::ERR_INTERNAL, "not reachable"};
    }
} setnxCmd;

class StrlenCommand: public Command {
 public:
    StrlenCommand()
        :Command("strlen") {
    }

    ssize_t arity() const {
        return 2;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);
        const std::string& key = sess->getArgs()[1];
        Expected<RecordValue> rv =
            Command::expireKeyIfNeeded(sess, key, RecordType::RT_KV);
        if (rv.status().code() == ErrorCodes::ERR_EXPIRED) {
            return Command::fmtZero();
        } else if (rv.status().code() == ErrorCodes::ERR_NOTFOUND) {
            return Command::fmtZero();
        } else if (!rv.status().ok()) {
            return rv.status();
        } else {
            return Command::fmtLongLong(rv.value().getValue().size());
        }
    }
} strlenCmd;

class BitPosCommand: public Command {
 public:
    BitPosCommand()
        :Command("bitpos") {
    }

    ssize_t arity() const {
        return -3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);
        const std::vector<std::string>& args = sess->getArgs();
        const std::string& key = args[1];
        uint32_t bit = 0;
        bool endGiven = false;
        if (args[2] == "0") {
            bit = 0;
        } else if (args[2] == "1") {
            bit = 1;
        } else {
            return {ErrorCodes::ERR_PARSEOPT,
                    "The bit argument must be 1 or 0."};
        }
        Expected<RecordValue> rv =
            Command::expireKeyIfNeeded(sess, key, RecordType::RT_KV);
        if (rv.status().code() == ErrorCodes::ERR_EXPIRED) {
            return Command::fmtLongLong(-1);
        } else if (rv.status().code() == ErrorCodes::ERR_NOTFOUND) {
            return Command::fmtLongLong(-1);
        } else if (!rv.status().ok()) {
            return rv.status();
        }
        int64_t start = 0;
        const std::string& target = rv.value().getValue();
        int64_t end = target.size() - 1;
        if (args.size() == 4 || args.size() == 5) {
            Expected<int64_t> estart = ::tendisplus::stoll(args[3]);
            if (!estart.ok()) {
                return estart.status();
            }
            start = estart.value();
            if (args.size() == 5) {
                Expected<int64_t> eend = ::tendisplus::stoll(args[4]);
                if (!eend.ok()) {
                    return eend.status();
                }
                end = eend.value();
                endGiven = true;
            }

            ssize_t len = rv.value().getValue().size();
            if (start < 0) {
                start = len + start;
            }
            if (end < 0) {
                end = len + end;
            }
            if (start < 0) {
                start = 0;
            }
            if (end < 0) {
                end = 0;
            }
            if (end >= len) {
                end = len - 1;
            }
        } else if (args.size() == 3) {
            // nothing
        } else {
            return {ErrorCodes::ERR_PARSEOPT, "syntax error"};
        }
        if (start > end) {
            return Command::fmtLongLong(-1);
        }
        int64_t result =
            redis_port::bitPos(target.c_str()+start, end-start+1, bit);
        if (endGiven && bit == 0 && result == (end-start+1)*8) {
            return Command::fmtLongLong(-1);
        }
        if (result != -1) {
            result += start*8;
        }
        return Command::fmtLongLong(result);
    }
} bitposCmd;

class BitCountCommand: public Command {
 public:
    BitCountCommand()
        :Command("bitcount") {
    }

    ssize_t arity() const {
        return -2;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);
        const std::string& key = sess->getArgs()[1];
        Expected<RecordValue> rv =
            Command::expireKeyIfNeeded(sess, key, RecordType::RT_KV);
        if (rv.status().code() == ErrorCodes::ERR_EXPIRED) {
            return Command::fmtZero();
        } else if (rv.status().code() == ErrorCodes::ERR_NOTFOUND) {
            return Command::fmtZero();
        } else if (!rv.status().ok()) {
            return rv.status();
        }
        int64_t start = 0;
        const std::string& target = rv.value().getValue();
        int64_t end = target.size() - 1;
        if (sess->getArgs().size() == 4) {
            Expected<int64_t> estart = ::tendisplus::stoll(sess->getArgs()[2]);
            Expected<int64_t> eend = ::tendisplus::stoll(sess->getArgs()[3]);
            if (!estart.ok() || !eend.ok()) {
                return estart.ok() ? eend.status() : estart.status();
            }
            start = estart.value();
            end = eend.value();
            ssize_t len = rv.value().getValue().size();
            if (start < 0) {
                start = len + start;
            }
            if (end < 0) {
                end = len + end;
            }
            if (start < 0) {
                start = 0;
            }
            if (end < 0) {
                end = 0;
            }
            if (end >= len) {
                end = len - 1;
            }
        } else if (sess->getArgs().size() == 2) {
            // nothing
        } else {
            return {ErrorCodes::ERR_PARSEOPT, "syntax error"};
        }
        if (start > end) {
            return Command::fmtZero();
        }
        return Command::fmtLongLong(
            redis_port::popCount(target.c_str() + start, end - start + 1));
    }
} bitcntCmd;

class GetGenericCmd: public Command {
 public:
    GetGenericCmd(const std::string& name)
        :Command(name) {
    }

    virtual Expected<std::string> run(Session *sess) {
        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);
        const std::string& key = sess->getArgs()[1];
        Expected<RecordValue> rv =
            Command::expireKeyIfNeeded(sess, key, RecordType::RT_KV);
        if (rv.status().code() == ErrorCodes::ERR_EXPIRED) {
            return std::string("");
        } else if (rv.status().code() == ErrorCodes::ERR_NOTFOUND) {
            return std::string("");
        } else if (!rv.status().ok()) {
            return rv.status();
        } else {
            return rv.value().getValue();
        }
    }
};

class GetVsnCommand: public Command {
 public:
    GetVsnCommand()
        :Command("getvsn") {
    }

    ssize_t arity() const {
        return 2;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);
        const std::string& key = sess->getArgs()[1];
        Expected<RecordValue> rv =
            Command::expireKeyIfNeeded(sess, key, RecordType::RT_KV);

        std::stringstream ss;
        Command::fmtMultiBulkLen(ss, 2);
        if (rv.status().code() == ErrorCodes::ERR_EXPIRED ||
                rv.status().code() == ErrorCodes::ERR_NOTFOUND) {
            Command::fmtLongLong(ss, -1);
            Command::fmtNull(ss);
            return ss.str();
        } else if (!rv.status().ok()) {
            return rv.status();
        } else {
            Command::fmtLongLong(ss, rv.value().getCas());
            if (rv.value().getValue() == "") {
                Command::fmtNull(ss);
            } else {
                Command::fmtBulk(ss, rv.value().getValue());
            }
            return ss.str();
        }
    }
} getvsnCmd;

class GetCommand: public GetGenericCmd {
 public:
    GetCommand()
        :GetGenericCmd("get") {
    }

    ssize_t arity() const {
        return 2;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        auto v = GetGenericCmd::run(sess);
        if (!v.ok()) {
            return v.status();
        }
        if (v.value() == "") {
            return Command::fmtNull();
        }
        return Command::fmtBulk(v.value());
    }
} getCommand;

// TODO(deyukong): unittest
class GetRangeGenericCommand: public GetGenericCmd {
 public:
    GetRangeGenericCommand(const std::string& name)
        :GetGenericCmd(name) {
    }

    ssize_t arity() const {
        return 4;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        Expected<int64_t> estart = ::tendisplus::stoll(sess->getArgs()[2]);
        if (!estart.ok()) {
            return estart.status();
        }
        int64_t start = estart.value();

        Expected<int64_t> eend = ::tendisplus::stoll(sess->getArgs()[3]);
        if (!eend.ok()) {
            return eend.status();
        }
        int64_t end = eend.value();

        auto v = GetGenericCmd::run(sess);
        if (!v.ok()) {
            return v.status();
        }
        std::string s = std::move(v.value());
        if (start < 0) {
            start = s.size() + start;
        }
        if (end < 0) {
            end = s.size() + end;
        }
        if (start < 0) {
            start = 0;
        }
        if (end < 0) {
            end = 0;
        }
        if (end >= static_cast<ssize_t>(s.size())) {
            end = s.size() - 1;
        }
        if (start > end || s.size() == 0) {
            return Command::fmtBulk("");
        }
        return Command::fmtBulk(s.substr(start, end-start+1));
    }
};

class GetRangeCommand: public GetRangeGenericCommand {
 public:
    GetRangeCommand()
        :GetRangeGenericCommand("getrange") {
    }
} getrangeCmd;

class Substrcommand: public GetRangeGenericCommand {
 public:
    Substrcommand()
        :GetRangeGenericCommand("substr") {
    }
} substrCmd;

class GetSetGeneral: public Command {
 public:
    explicit GetSetGeneral(const std::string& name)
        :Command(name) {
    }

    virtual bool replyNewValue() const {
        return true;
    }

    virtual Expected<RecordValue> newValueFromOld(Session* sess,
                            const Expected<RecordValue>& oldValue) const = 0;

    Expected<RecordValue> runGeneral(Session *sess) {
            const std::string& key = sess->getArgs()[firstkey()];
        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);

        // expire if possible
        Expected<RecordValue> rv =
            Command::expireKeyIfNeeded(sess, key, RecordType::RT_KV);
        if (rv.status().code() != ErrorCodes::ERR_OK &&
                rv.status().code() != ErrorCodes::ERR_EXPIRED &&
                rv.status().code() != ErrorCodes::ERR_NOTFOUND) {
            return rv.status();
        }

        auto server = sess->getServerEntry();
        INVARIANT(server != nullptr);
        auto expdb = server->getSegmentMgr()->getDbWithKeyLock(sess, key, mgl::LockMode::LOCK_X);
        if (!expdb.ok()) {
            return expdb.status();
        }
        PStore kvstore = expdb.value().store;
        RecordKey rk(expdb.value().chunkId, pCtx->getDbId(), RecordType::RT_KV, key, "");

        for (int32_t i = 0; i < RETRY_CNT; ++i) {
            auto ptxn = kvstore->createTransaction();
            if (!ptxn.ok()) {
                return ptxn.status();
            }
            std::unique_ptr<Transaction> txn = std::move(ptxn.value());
            Expected<RecordValue> eValue = kvstore->getKV(rk, txn.get());
            if (!eValue.ok()
                    && eValue.status().code() != ErrorCodes::ERR_NOTFOUND) {
                return eValue.status();
            }
            const Expected<RecordValue>& newValue = newValueFromOld(sess, eValue);
            if (!newValue.ok()) {
                return newValue.status();
            }
            auto result = setGeneric(kvstore,
                                     txn.get(),
                                     REDIS_SET_NO_FLAGS,
                                     rk, newValue.value(), "", "");
            if (result.ok()) {
                if (replyNewValue()) {
                    return std::move(newValue.value());
                } else {
                    return eValue.ok() ?
                           std::move(eValue.value()) : RecordValue("");
                }
            }
            if (result.status().code() != ErrorCodes::ERR_COMMIT_RETRY) {
                return result.status();
            }
            if (i == RETRY_CNT - 1) {
                return result.status();
            } else {
                continue;
            }
        }

        INVARIANT(0);
        return {ErrorCodes::ERR_INTERNAL, "not reachable"};
    }
};

class CasCommand: public GetSetGeneral {
 public:
    CasCommand()
        :GetSetGeneral("cas") {
    }

    ssize_t arity() const {
        return 4;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<RecordValue> newValueFromOld(Session* sess,
                          const Expected<RecordValue>& oldValue) const {
        Expected<uint64_t> ecas = ::tendisplus::stoul(sess->getArgs()[2]);
        if (!ecas.ok()) {
            return ecas.status();
        }

        RecordValue ret(sess->getArgs()[3]);
        if (!oldValue.ok()) {
            ret.setCas(ecas.value());
            return ret;
        }

        if (ecas.value() != oldValue.value().getCas()) {
            return {ErrorCodes::ERR_CAS, "cas unmatch"};
        }

        ret.setCas(ecas.value() + 1);
        ret.setTtl(oldValue.value().getTtl());
        return std::move(ret);
    }

    Expected<std::string> run(Session *sess) final {
        const Expected<RecordValue>& rv = runGeneral(sess);
        if (!rv.ok()) {
            return rv.status();
        }
        return Command::fmtOK();
    }
} casCommand;

class AppendCommand: public GetSetGeneral {
 public:
    AppendCommand()
        :GetSetGeneral("append") {
    }

    ssize_t arity() const {
        return 3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<RecordValue> newValueFromOld(Session* sess,
                          const Expected<RecordValue>& oldValue) const {
        const std::string& val = sess->getArgs()[2];
        std::string cat;
        if (!oldValue.ok()) {
            cat = val;
        } else {
            cat = oldValue.value().getValue();
            cat.insert(cat.end(), val.begin(), val.end());
        }

        uint64_t ttl = 0;
        if (oldValue.ok()) {
            ttl = oldValue.value().getTtl();
        }
        return std::move(RecordValue(std::move(cat), ttl));
    }

    Expected<std::string> run(Session *sess) final {
        const Expected<RecordValue>& rv = runGeneral(sess);
        if (!rv.ok()) {
            return rv.status();
        }
        return Command::fmtLongLong(rv.value().getValue().size());
    }
} appendCmd;

// TODO(unittest)
class SetRangeCommand: public GetSetGeneral {
 public:
    SetRangeCommand()
        :GetSetGeneral("setrange") {
    }

    ssize_t arity() const {
        return 4;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<RecordValue> newValueFromOld(Session* sess,
                          const Expected<RecordValue>& oldValue) const {
        const std::string& val = sess->getArgs()[3];
        Expected<int64_t> eoffset = ::tendisplus::stoll(sess->getArgs()[2]);
        if (!eoffset.ok()) {
            return eoffset.status();
        }
        if (eoffset.value() < 0) {
            return {ErrorCodes::ERR_PARSEOPT, "offset is out of range"};
        }
        uint32_t offset = eoffset.value();
        if (offset + val.size() > 512*1024*1024) {
            return {ErrorCodes::ERR_PARSEOPT,
                    "string exceeds maximum allowed size (512MB)"};
        }
        std::string cat;
        if (oldValue.ok()) {
            cat = oldValue.value().getValue();
        }
        if (offset + val.size() > cat.size()) {
            cat.resize(offset + val.size(), 0);
        }
        for (size_t i = offset; i < offset + val.size(); ++i) {
            cat[i] = val[i-offset];
        }
        uint64_t ttl = 0;
        if (oldValue.ok()) {
            ttl = oldValue.value().getTtl();
        }
        return RecordValue(std::move(cat), ttl);
    }

    Expected<std::string> run(Session *sess) final {
        const Expected<RecordValue>& rv = runGeneral(sess);
        if (!rv.ok()) {
            return rv.status();
        }
        return Command::fmtLongLong(rv.value().getValue().size());
    }
} setrangeCmd;

class SetBitCommand: public GetSetGeneral {
 public:
    SetBitCommand()
        :GetSetGeneral("setbit") {
    }

    bool replyNewValue() const final {
        return false;
    }

    ssize_t arity() const {
        return 4;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<RecordValue> newValueFromOld(Session* sess,
                          const Expected<RecordValue>& oldValue) const {
        Expected<uint64_t> epos = ::tendisplus::stoul(sess->getArgs()[2]);
        if (!epos.ok()) {
            return epos.status();
        }
        uint64_t pos = epos.value();
        int on = 0;
        std::string tomodify;
        if (oldValue.ok()) {
            tomodify = oldValue.value().getValue();
        }
        if ((pos >> 3) >= (512*1024*1024)) {
            return {ErrorCodes::ERR_PARSEOPT,
                    "bit offset is not an integer or out of range"};
        }
        if ((pos >> 3) > 4*1024*1024) {
            LOG(WARNING) << "meet large bitpos:" << pos;
        }
        if (sess->getArgs()[3] == "1") {
            on = 1;
        } else if (sess->getArgs()[3] == "0") {
            on = 0;
        } else {
            return {ErrorCodes::ERR_PARSEOPT,
                    "bit is not an integer or out of range"};
        }

        uint64_t byte = (pos>>3);
        if (tomodify.size() < byte+1) {
            tomodify.resize(byte+1, 0);
        }
        uint8_t byteval = static_cast<uint8_t>(tomodify[byte]);
        uint8_t bit = 7 - (pos & 0x7);
        byteval &= ~(1 << bit);
        byteval |= ((on & 0x1) << bit);
        tomodify[byte] = byteval;

        // incrby wont clear ttl
        uint64_t ttl = 0;
        if (oldValue.ok()) {
            ttl = oldValue.value().getTtl();
        }
        return RecordValue(std::move(tomodify), ttl);
    }

    Expected<std::string> run(Session *sess) final {
        const Expected<RecordValue>& rv = runGeneral(sess);
        if (!rv.ok()) {
            return rv.status();
        }

        Expected<uint64_t> epos = ::tendisplus::stoul(sess->getArgs()[2]);
        if (!epos.ok()) {
            return epos.status();
        }
        uint64_t pos = epos.value();
        std::string toreturn = rv.value().getValue();

        uint64_t byte = (pos>>3);
        if (toreturn.size() < byte+1) {
            toreturn.resize(byte+1, 0);
        }
        uint8_t byteval = static_cast<uint8_t>(toreturn[byte]);
        uint8_t bit = 7 - (pos & 0x7);
        uint8_t bitval = byteval & (1 << bit);
        return bitval ? Command::fmtOne() : Command::fmtZero();
    }
} setbitCmd;

class GetSetCommand: public GetSetGeneral {
 public:
    GetSetCommand()
        :GetSetGeneral("getset") {
    }

    bool replyNewValue() const final {
        return false;
    }

    ssize_t arity() const {
        return 3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<RecordValue> newValueFromOld(Session* sess,
                          const Expected<RecordValue>& oldValue) const {
        (void)oldValue;
        // getset overwrites ttl
        return RecordValue(sess->getArgs()[2], 0);
    }

    Expected<std::string> run(Session *sess) final {
        const Expected<RecordValue>& rv = runGeneral(sess);
        if (!rv.ok()) {
            return rv.status();
        }
        const std::string& v = rv.value().getValue();
        if (v.size()) {
            return Command::fmtBulk(v);
        }
        return Command::fmtNull();
    }
} getsetCmd;

class IncrDecrGeneral: public GetSetGeneral {
 public:
    explicit IncrDecrGeneral(const std::string& name)
        :GetSetGeneral(name) {
    }

    Expected<int64_t> sumIncr(const Expected<RecordValue>& esum,
                              int64_t incr) const {
        int64_t sum = 0;
        if (esum.ok()) {
            Expected<int64_t> val =
                ::tendisplus::stoll(esum.value().getValue());
            if (!val.ok()) {
                return {ErrorCodes::ERR_DECODE,
                        "value is not an integer or out of range"};
            }
            sum = val.value();
        }

        if ((incr < 0 && sum < 0 && incr < (LLONG_MIN-sum)) ||
                (incr > 0 && sum > 0 && incr > (LLONG_MAX-sum))) {
            return {ErrorCodes::ERR_OVERFLOW,
                    "increment or decrement would overflow"};
        }
        sum += incr;
        return sum;
    }

    virtual Expected<std::string> run(Session *sess) {
        const Expected<RecordValue>& rv = runGeneral(sess);
        if (!rv.ok()) {
            return rv.status();
        }
        Expected<int64_t> val = ::tendisplus::stoll(rv.value().getValue());
        if (!val.ok()) {
            return val.status();
        }
        return Command::fmtLongLong(val.value());
    }
};

class IncrbyfloatCommand: public GetSetGeneral {
 public:
    IncrbyfloatCommand()
        :GetSetGeneral("incrbyfloat") {
    }

    ssize_t arity() const {
        return 3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<long double> sumIncr(const Expected<RecordValue>& esum,
                              long double incr) const {
        long double sum = 0;
        if (esum.ok()) {
            Expected<long double> val =
                ::tendisplus::stold(esum.value().getValue());
            if (!val.ok()) {
                return {ErrorCodes::ERR_DECODE, "value is not double"};
            }
            sum = val.value();
        }

        sum += incr;
        return sum;
    }

    virtual Expected<std::string> run(Session *sess) {
        const Expected<RecordValue>& rv = runGeneral(sess);
        if (!rv.ok()) {
            return rv.status();
        }
        Expected<long double> val = ::tendisplus::stold(rv.value().getValue());
        if (!val.ok()) {
            return val.status();
        }
        return Command::fmtBulk(redis_port::ldtos(val.value()));
    }

    Expected<RecordValue> newValueFromOld(
            Session* sess, const Expected<RecordValue>& oldValue) const {
        const std::string& val = sess->getArgs()[2];
        Expected<long double> eInc = ::tendisplus::stold(val);
        if (!eInc.ok()) {
            return eInc.status();
        }
        Expected<long double> newSum = sumIncr(oldValue, eInc.value());
        if (!newSum.ok()) {
            return newSum.status();
        }

        // incrby wont clear ttl
        uint64_t ttl = 0;
        if (oldValue.ok()) {
            ttl = oldValue.value().getTtl();
        }
        return RecordValue(redis_port::ldtos(newSum.value()), ttl);
    }
} incrbyfloatCmd;

class IncrbyCommand: public IncrDecrGeneral {
 public:
    IncrbyCommand()
        :IncrDecrGeneral("incrby") {
    }

    ssize_t arity() const {
        return 3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<RecordValue> newValueFromOld(
            Session* sess, const Expected<RecordValue>& oldValue) const {
        const std::string& val = sess->getArgs()[2];
        Expected<int64_t> eInc = ::tendisplus::stoll(val);
        if (!eInc.ok()) {
            return eInc.status();
        }
        Expected<int64_t> newSum = sumIncr(oldValue, eInc.value());
        if (!newSum.ok()) {
            return newSum.status();
        }

        // incrby wont clear ttl
        uint64_t ttl = 0;
        if (oldValue.ok()) {
            ttl = oldValue.value().getTtl();
        }
        return RecordValue(std::to_string(newSum.value()), ttl);
    }
} incrbyCmd;

class IncrCommand: public IncrDecrGeneral {
 public:
    IncrCommand()
        :IncrDecrGeneral("incr") {
    }
    ssize_t arity() const {
        return 2;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<RecordValue> newValueFromOld(Session* sess,
                          const Expected<RecordValue>& oldValue) const {
        Expected<int64_t> newSum = sumIncr(oldValue, 1);
        if (!newSum.ok()) {
            return newSum.status();
        }

        // incrby wont clear ttl
        uint64_t ttl = 0;
        if (oldValue.ok()) {
            ttl = oldValue.value().getTtl();
        }
        return RecordValue(std::to_string(newSum.value()), ttl);
    }
} incrCmd;

class DecrbyCommand: public IncrDecrGeneral {
 public:
    DecrbyCommand()
        :IncrDecrGeneral("decrby") {
    }

    ssize_t arity() const {
        return 3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<RecordValue> newValueFromOld(Session* sess,
                          const Expected<RecordValue>& oldValue) const {
        const std::string& val = sess->getArgs()[2];
        Expected<int64_t> eInc = ::tendisplus::stoll(val);
        if (!eInc.ok()) {
            return eInc.status();
        }
        Expected<int64_t> newSum = sumIncr(oldValue, -eInc.value());
        if (!newSum.ok()) {
            return newSum.status();
        }

        // incrby wont clear ttl
        uint64_t ttl = 0;
        if (oldValue.ok()) {
            ttl = oldValue.value().getTtl();
        }
        LOG(INFO) << "decr new val:" << newSum.value() << ' ' << val;
        return RecordValue(std::to_string(newSum.value()), ttl);
    }
} decrbyCmd;

class DecrCommand: public IncrDecrGeneral {
 public:
    DecrCommand()
        :IncrDecrGeneral("decr") {
    }

    ssize_t arity() const {
        return 2;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<RecordValue> newValueFromOld(Session* sess,
                          const Expected<RecordValue>& oldValue) const {
        Expected<int64_t> newSum = sumIncr(oldValue, -1);
        if (!newSum.ok()) {
            return newSum.status();
        }

        // incrby wont clear ttl
        uint64_t ttl = 0;
        if (oldValue.ok()) {
            ttl = oldValue.value().getTtl();
        }
        return RecordValue(std::to_string(newSum.value()), ttl);
    }
} decrCmd;

class MGetCommand: public Command {
 public:
    MGetCommand()
        :Command("mget") {
    }

    ssize_t arity() const {
        return -2;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);

        std::stringstream ss;
        Command::fmtMultiBulkLen(ss, sess->getArgs().size()-1);
        for (size_t i = 1; i < sess->getArgs().size(); ++i) {
            const std::string& key = sess->getArgs()[i];
            Expected<RecordValue> rv =
                Command::expireKeyIfNeeded(sess, key, RecordType::RT_KV);
            if (rv.status().code() == ErrorCodes::ERR_EXPIRED) {
                Command::fmtNull(ss);
                continue;
            } else if (rv.status().code() == ErrorCodes::ERR_NOTFOUND) {
                Command::fmtNull(ss);
                continue;
            } else if (!rv.status().ok()) {
                return rv.status();
            }
            Command::fmtBulk(ss, rv.value().getValue());
        }
        return ss.str();
    }
} mgetCmd;

class BitopCommand: public Command {
 public:
    BitopCommand()
        :Command("bitop") {
    }

    enum class Op {
        BITOP_AND,
        BITOP_OR,
        BITOP_XOR,
        BITOP_NOT,
    };

    ssize_t arity() const {
        return -4;
    }

    int32_t firstkey() const {
        return 2;
    }

    int32_t lastkey() const {
        return -1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        const auto& args = sess->getArgs();
        const std::string& opName = args[1];
        const std::string& targetKey = args[2];
        Op op;
        if (opName == "and" || opName == "AND") {
            op = Op::BITOP_AND;
        } else if (opName == "or" || opName == "OR") {
            op = Op::BITOP_OR;
        } else if (opName == "xor" || opName == "XOR") {
            op = Op::BITOP_XOR;
        } else if (opName == "not" || opName == "NOT") {
            op = Op::BITOP_NOT;
        } else {
            return {ErrorCodes::ERR_PARSEPKT, "syntax error"};
        }
        if (op == Op::BITOP_NOT && args.size() != 4) {
            return {ErrorCodes::ERR_PARSEPKT, "BITOP NOT must be called with a single source key."};
        }
        size_t numKeys = args.size() - 3;
        size_t maxLen = 0;
        std::vector<std::string> vals;
        for (size_t j = 0; j < numKeys; ++j) {
            Expected<RecordValue> rv =
                Command::expireKeyIfNeeded(sess, args[j+3], RecordType::RT_KV);
            if (rv.status().code() == ErrorCodes::ERR_EXPIRED) {
                vals.push_back(rv.value().getValue());
            } else if (rv.status().code() == ErrorCodes::ERR_NOTFOUND) {
                vals.push_back(rv.value().getValue());
            } else if (!rv.status().ok()) {
                return rv.status();
            } else {
                vals.push_back(rv.value().getValue());
                if (vals[j].size() > maxLen) {
                    maxLen = vals[j].size();
                }
            }
        }
        if (maxLen == 0) {
            Command::delKeyChkExpire(sess, targetKey, RecordType::RT_KV);
            return Command::fmtZero();
        }
        std::string result(maxLen, 0);
        for (size_t i = 0; i < maxLen; ++i) {
            unsigned char output = (vals[0].size() <= i) ? 0 : vals[0][i];
            if (op == Op::BITOP_NOT) output = ~output;
            for (size_t j = 1; j < numKeys; ++j) {
                unsigned char byte = (vals[j].size() <= i) ? 0 : vals[j][i];
                switch (op) {
                    case Op::BITOP_AND: output &= byte; break;
                    case Op::BITOP_OR: output |= byte; break;
                    case Op::BITOP_XOR: output ^= byte; break;
                    default:
                        INVARIANT(0);
                }
            }
            result[i] = output;
        }

        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);
        auto server = sess->getServerEntry();
        INVARIANT(server != nullptr);
        auto expdb = server->getSegmentMgr()->getDbWithKeyLock(sess, targetKey, mgl::LockMode::LOCK_X);
        if (!expdb.ok()) {
            return expdb.status();
        }
        PStore kvstore = expdb.value().store;

        RecordKey rk(expdb.value().chunkId, pCtx->getDbId(), RecordType::RT_KV, targetKey, "");
        RecordValue rv(result);
        for (int32_t i = 0; i < RETRY_CNT; ++i) {
            auto ptxn = kvstore->createTransaction();
            if (!ptxn.ok()) {
                return ptxn.status();
            }
            std::unique_ptr<Transaction> txn = std::move(ptxn.value());
            auto setRes = setGeneric(kvstore,
                                     txn.get(),
                                     REDIS_SET_NO_FLAGS,
                                     rk,
                                     rv,
                                     "",
                                     "");
            if (setRes.ok()) {
                return Command::fmtLongLong(result.size());
            }
            if (setRes.status().code() != ErrorCodes::ERR_COMMIT_RETRY) {
                return setRes.status();
            }
            if (i == RETRY_CNT - 1) {
                return setRes.status();
            } else {
                continue;
            }
        }
        INVARIANT(0);
        return {ErrorCodes::ERR_INTERNAL, "not reachable"};
    }
} bitopCmd;

// NOTE(deyukong): redis guarantees mset is atomic. not partially
// visible to other clients, to implement this, we can take all
// related stores' X lock to do this. we didnt do this for better
// performance.
// NOTE(deyukong): we commit kv one by one, so there is a chance
// that this command partial successes.
class MSetCommand: public Command {
 public:
    MSetCommand()
        :Command("mset") {
    }

    ssize_t arity() const {
        return -3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return -1;
    }

    int32_t keystep() const {
        return 2;
    }

    Expected<std::string> run(Session *sess) final {
        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);

        for (size_t i = 1; i < sess->getArgs().size(); i+= 2) {
            const std::string& key = sess->getArgs()[i];
            const std::string& val = sess->getArgs()[i+1];
            auto server = sess->getServerEntry();
            INVARIANT(server != nullptr);
            auto expdb = server->getSegmentMgr()->getDbWithKeyLock(sess, key, mgl::LockMode::LOCK_X);
            if (!expdb.ok()) {
                return expdb.status();
            }
            PStore kvstore = expdb.value().store;

            RecordKey rk(expdb.value().chunkId, pCtx->getDbId(), RecordType::RT_KV, key, "");
            RecordValue rv(val);
            for (int32_t i = 0; i < RETRY_CNT; ++i) {
                auto ptxn = kvstore->createTransaction();
                if (!ptxn.ok()) {
                    return ptxn.status();
                }
                std::unique_ptr<Transaction> txn = std::move(ptxn.value());
                auto result = setGeneric(kvstore,
                                         txn.get(),
                                         REDIS_SET_NO_FLAGS,
                                         rk,
                                         rv,
                                         "",
                                         "");
                if (result.ok()) {
                    break;
                }
                if (result.status().code() != ErrorCodes::ERR_COMMIT_RETRY) {
                    return result.status();
                }
                if (i == RETRY_CNT - 1) {
                    return result.status();
                } else {
                    continue;
                }
            }
        }
        return Command::fmtOK();
    }
} msetCmd;

class MoveCommand: public Command {
 public:
    MoveCommand()
        :Command("move") {
    }

    ssize_t arity() const {
        return 3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        return {ErrorCodes::ERR_INTERNAL, "not support"};
    }
} moveCmd;

class RenameCommand: public Command {
 public:
    RenameCommand()
        :Command("rename") {
    }

    ssize_t arity() const {
        return 3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 2;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        return {ErrorCodes::ERR_INTERNAL, "not support"};
    }
} renameCmd;

class RenamenxCommand: public Command {
 public:
    RenamenxCommand()
        :Command("renamenx") {
    }

    ssize_t arity() const {
        return 3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 2;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        return {ErrorCodes::ERR_INTERNAL, "not support"};
    }
} renamenxCmd;

}  // namespace tendisplus