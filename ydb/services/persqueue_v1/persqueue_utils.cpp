#include "persqueue_utils.h"

#include <ydb/core/base/path.h>

namespace NKikimr::NGRpcProxy::V1 {

TAclWrapper::TAclWrapper(THolder<NACLib::TSecurityObject> acl)
    : AclOldSchemeCache(std::move(acl))
{
    Y_VERIFY(AclOldSchemeCache);
}

TAclWrapper::TAclWrapper(TIntrusivePtr<TSecurityObject> acl)
    : AclNewSchemeCache(std::move(acl))
{
    Y_VERIFY(AclNewSchemeCache);
}

bool TAclWrapper::CheckAccess(NACLib::EAccessRights rights, const NACLib::TUserToken& userToken) {
    if (AclOldSchemeCache) {
        return AclOldSchemeCache->CheckAccess(rights, userToken);
    } else {
        return AclNewSchemeCache->CheckAccess(rights, userToken);
    }
}

using namespace NSchemeCache;

TProcessingResult ProcessMetaCacheTopicResponse(const TSchemeCacheNavigate::TEntry& entry) {
    auto fullPath = JoinPath(entry.Path);
    auto& topicName = entry.Path.back();
    switch (entry.Status) {
        case TSchemeCacheNavigate::EStatus::RootUnknown : {
            return TProcessingResult {
                    Ydb::PersQueue::ErrorCode::ErrorCode::BAD_REQUEST,
                    Sprintf("path '%s' has unknown/invalid root prefix '%s', Marker# PQ14",
                            fullPath.c_str(), entry.Path[0].c_str()),
                    true
            };
        }
        case TSchemeCacheNavigate::EStatus::PathErrorUnknown: {
            return TProcessingResult {
                    Ydb::PersQueue::ErrorCode::ErrorCode::UNKNOWN_TOPIC,
                    Sprintf("no path '%s', Marker# PQ15", fullPath.c_str()),
                    true
            };
        }
        case TSchemeCacheNavigate::EStatus::Ok:
            break;
        default: {
            return TProcessingResult {
                    Ydb::PersQueue::ErrorCode::ErrorCode::ERROR,
                    Sprintf("topic '%s' describe error, Status# %s, Marker# PQ1",
                            topicName.c_str(), ToString(entry.Status).c_str()),
                    true
            };
        }
    }

    if (entry.Kind != TSchemeCacheNavigate::KindTopic) {
        return TProcessingResult {
                Ydb::PersQueue::ErrorCode::ErrorCode::UNKNOWN_TOPIC,
                Sprintf("item '%s' is not a topic, Marker# PQ13", fullPath.c_str()),
                true
        };
    }
    if (!entry.PQGroupInfo) {
        return TProcessingResult {
                Ydb::PersQueue::ErrorCode::ErrorCode::ERROR,
                Sprintf("topic '%s' describe error, reason: could not retrieve topic description, Marker# PQ99",
                        topicName.c_str()),
                true
        };
    }
    auto& description = entry.PQGroupInfo->Description;
    if (!description.HasBalancerTabletID() || description.GetBalancerTabletID() == 0) {
        return TProcessingResult {
                Ydb::PersQueue::ErrorCode::ErrorCode::UNKNOWN_TOPIC,
                Sprintf("topic '%s' has no balancer, Marker# PQ193", topicName.c_str()),
                true
        };
    }
    return {};
}

Ydb::StatusIds::StatusCode ConvertPersQueueInternalCodeToStatus(const Ydb::PersQueue::ErrorCode::ErrorCode code) {

    using namespace Ydb::PersQueue::ErrorCode;

    switch(code) {
        case OK :
            return Ydb::StatusIds::SUCCESS;
        case INITIALIZING:
        case CLUSTER_DISABLED:
            return Ydb::StatusIds::UNAVAILABLE;
        case PREFERRED_CLUSTER_MISMATCHED:
            return Ydb::StatusIds::ABORTED;
        case OVERLOAD:
            return Ydb::StatusIds::OVERLOADED;
        case BAD_REQUEST:
            return Ydb::StatusIds::BAD_REQUEST;
        case WRONG_COOKIE:
        case CREATE_SESSION_ALREADY_LOCKED:
        case DELETE_SESSION_NO_SESSION:
        case READ_ERROR_NO_SESSION:
            return Ydb::StatusIds::SESSION_EXPIRED;
        case WRITE_ERROR_PARTITION_IS_FULL:
        case WRITE_ERROR_DISK_IS_FULL:
        case WRITE_ERROR_BAD_OFFSET:
        case SOURCEID_DELETED:
        case READ_ERROR_IN_PROGRESS:
        case READ_ERROR_TOO_SMALL_OFFSET:
        case READ_ERROR_TOO_BIG_OFFSET:
        case SET_OFFSET_ERROR_COMMIT_TO_FUTURE:
        case READ_NOT_DONE:
            return Ydb::StatusIds::GENERIC_ERROR;
        case TABLET_IS_DROPPED:
        case UNKNOWN_TOPIC:
        case WRONG_PARTITION_NUMBER:
            return Ydb::StatusIds::SCHEME_ERROR;
        case ACCESS_DENIED:
            return Ydb::StatusIds::UNAUTHORIZED;
        case ERROR:
            return Ydb::StatusIds::GENERIC_ERROR;

        default:
            return Ydb::StatusIds::STATUS_CODE_UNSPECIFIED;
    }
}

Ydb::StatusIds::StatusCode ConvertPersQueueInternalCodeToStatus(const NPersQueue::NErrorCode::EErrorCode code)
{
    using namespace NPersQueue::NErrorCode;

    switch(code) {
        case OK :
            return Ydb::StatusIds::SUCCESS;
        case INITIALIZING:
        case CLUSTER_DISABLED:
            return Ydb::StatusIds::UNAVAILABLE;
        case OVERLOAD:
            return Ydb::StatusIds::OVERLOADED;
        case BAD_REQUEST:
            return Ydb::StatusIds::BAD_REQUEST;
        case WRONG_COOKIE:
        case CREATE_SESSION_ALREADY_LOCKED:
        case DELETE_SESSION_NO_SESSION:
        case READ_ERROR_NO_SESSION:
            return Ydb::StatusIds::SESSION_EXPIRED;
        case WRITE_ERROR_PARTITION_IS_FULL:
        case WRITE_ERROR_DISK_IS_FULL:
        case WRITE_ERROR_BAD_OFFSET:
        case SOURCEID_DELETED:
        case READ_ERROR_IN_PROGRESS:
        case READ_ERROR_TOO_SMALL_OFFSET:
        case READ_ERROR_TOO_BIG_OFFSET:
        case SET_OFFSET_ERROR_COMMIT_TO_FUTURE:
        case READ_NOT_DONE:
            return Ydb::StatusIds::GENERIC_ERROR;
        case TABLET_IS_DROPPED:
        case UNKNOWN_TOPIC:
        case WRONG_PARTITION_NUMBER:
            return Ydb::StatusIds::SCHEME_ERROR;
        case ACCESS_DENIED:
            return Ydb::StatusIds::UNAUTHORIZED;
        case ERROR:
            return Ydb::StatusIds::GENERIC_ERROR;

        default:
            return Ydb::StatusIds::STATUS_CODE_UNSPECIFIED;
    }
}

} // namespace NKikimr::NGRpcProxy::V1
