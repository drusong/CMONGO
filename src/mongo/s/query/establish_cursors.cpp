/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/s/query/establish_cursors.h"

#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/killcursors_request.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

std::vector<ClusterClientCursorParams::RemoteCursor> establishCursors(
    OperationContext* opCtx,
    executor::TaskExecutor* executor,
    const NamespaceString& nss,
    const ReadPreferenceSetting readPref,
    const std::vector<std::pair<ShardId, BSONObj>>& remotes,
    bool allowPartialResults) {
    // Construct the requests
    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& remote : remotes) {
        requests.emplace_back(remote.first, remote.second);
    }

    // Send the requests
    AsyncRequestsSender ars(opCtx,
                            executor,
                            nss.db().toString(),
                            std::move(requests),
                            readPref,
                            Shard::RetryPolicy::kIdempotent);

    std::vector<ClusterClientCursorParams::RemoteCursor> remoteCursors;
    try {
        // Get the responses
        while (!ars.done()) {
            try {
                auto response = ars.next();

                // uasserts must happen before attempting to access the optional shardHostAndPort.
                auto cursorResponse = uassertStatusOK(CursorResponse::parseFromBSON(
                    uassertStatusOK(std::move(response.swResponse)).data));

                remoteCursors.emplace_back(std::move(response.shardId),
                                           std::move(*response.shardHostAndPort),
                                           std::move(cursorResponse));
            } catch (const DBException& ex) {
                // Retriable errors are swallowed if 'allowPartialResults' is true.
                if (allowPartialResults &&
                    std::find(RemoteCommandRetryScheduler::kAllRetriableErrors.begin(),
                              RemoteCommandRetryScheduler::kAllRetriableErrors.end(),
                              ex.code()) !=
                        RemoteCommandRetryScheduler::kAllRetriableErrors.end()) {
                    continue;
                }
                throw;  // Fail this loop.
            }
        }
        return remoteCursors;
    } catch (const DBException&) {
        // If one of the remotes had an error, we make a best effort to finish retrieving responses
        // for other requests that were already sent, so that we can send killCursors to any cursors
        // that we know were established.
        try {
            // Do not schedule any new requests.
            ars.stopRetrying();

            // Collect responses from all requests that were already sent.
            while (!ars.done()) {
                auto response = ars.next();

                // Check if the response contains an established cursor, and if so, store it.
                StatusWith<CursorResponse> swCursorResponse(
                    response.swResponse.isOK()
                        ? CursorResponse::parseFromBSON(response.swResponse.getValue().data)
                        : response.swResponse.getStatus());

                if (swCursorResponse.isOK()) {
                    remoteCursors.emplace_back(std::move(response.shardId),
                                               *response.shardHostAndPort,
                                               std::move(swCursorResponse.getValue()));
                }
            }

            // Schedule killCursors against all cursors that were established.
            for (const auto& remoteCursor : remoteCursors) {
                BSONObj cmdObj =
                    KillCursorsRequest(nss, {remoteCursor.cursorResponse.getCursorId()}).toBSON();
                executor::RemoteCommandRequest request(
                    remoteCursor.hostAndPort, nss.db().toString(), cmdObj, opCtx);

                // We do not process the response to the killCursors request (we make a good-faith
                // attempt at cleaning up the cursors, but ignore any returned errors).
                executor
                    ->scheduleRemoteCommand(
                        request,
                        [](const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData) {})
                    .status_with_transitional_ignore();
            }
        } catch (const DBException&) {
            // Ignore the new error and rethrow the original one.
        }

        throw;
    }
}

}  // namespace mongo
