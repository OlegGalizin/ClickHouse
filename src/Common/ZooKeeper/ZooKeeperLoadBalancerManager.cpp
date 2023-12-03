#include <memory>
#include "Common/logger_useful.h"
#include <Common/ZooKeeper/ZooKeeperLoadBalancerManager.h>
#include <Common/ZooKeeper/ZooKeeperImpl.h>
#include "Interpreters/DDLTask.h"
#include "Interpreters/SystemLog.h"
#include "base/types.h"

namespace Coordination
{

std::pair<std::string, bool> parseForSocketAddress(const std::string & raw_host)
{
    std::pair<std::string, bool> result;
    bool secure = startsWith(raw_host, "secure://");
    if (secure)
        result.first = raw_host.substr(strlen("secure://"));
    else
        result.first = raw_host;
    result.second = secure;
    return result;
}

// std::move(args_) not working, later on some how the hosts count is zero. maybe there're a few rounds of init. TBD.
ZooKeeperLoadBalancerManager::ZooKeeperLoadBalancerManager(zkutil::ZooKeeperArgs args_, std::shared_ptr<ZooKeeperLog> zk_log_)
    : args(args_), zk_log(std::move(zk_log_))
{
    log = &Poco::Logger::get("ZooKeeperLoadBalancerManager");
    LOG_INFO(log, "Initializing ZooKeeperLoadBalancerManager with hosts {}", args.hosts.size());
    for (size_t i = 0; i < args_.hosts.size(); ++i)
    {
        HostInfo host_info;
        auto [address, secure] =  parseForSocketAddress(args_.hosts[i]);
        host_info.host = address;
        host_info.original_index = i;
        host_info.secure = secure;
        host_info_list.push_back(host_info);
    }
}


void ZooKeeperLoadBalancerManager::initialize()
{
}


[[noreturn]] void throwWhenNoHostAvailable(bool dns_error_occurred)
{
    if (dns_error_occurred)
        throw zkutil::KeeperException::fromMessage(Coordination::Error::ZCONNECTIONLOSS, "Cannot resolve any of provided ZooKeeper hosts due to DNS error");
    else
        throw zkutil::KeeperException::fromMessage(Coordination::Error::ZCONNECTIONLOSS, "Cannot use any of provided ZooKeeper nodes");
}


void ZooKeeperLoadBalancerManager::shuffleHosts()
{
    std::function<Priority(size_t index)> get_priority = args.get_priority_load_balancing.getPriorityFunc(args.get_priority_load_balancing.load_balancing, 0, args.hosts.size());
    for (size_t i = 0; i < host_info_list.size(); ++i)
    {
        // if (!isKeeperHostDNSAvailable(log, args.hosts[i], dns_error_occurred))
        //     continue;
        if (get_priority)
            host_info_list[i].priority = get_priority(i);
        host_info_list[i].randomize();
    }

    ::sort(host_info_list.begin(), host_info_list.end(), HostInfo::compare);
}

std::unique_ptr<Coordination::ZooKeeper> ZooKeeperLoadBalancerManager::createClient()
{
    shuffleHosts();
    for (size_t i = 0; i < host_info_list.size(); ++i)
    {
        LOG_INFO(log, "Connecting to ZooKeeper host {}, number of attempted hosts {}", host_info_list[i].host, i+1);
        try
        {
            Coordination::ZooKeeper::Nodes nodes;
            nodes.emplace_back(host_info_list[i].toZooKeeperNode());
            auto client  = std::make_unique<Coordination::ZooKeeper>(nodes, args, zk_log);

            // Non optimal case: we connected to a keeper host in different availability zone, so set a session deadline.
            if (i != 0)
            {
                auto session_timeout_seconds = client->setClientSessionDeadline(
                    args.fallback_session_lifetime.min_sec, args.fallback_session_lifetime.max_sec);
                LOG_INFO(log, "Connecting to a different az ZooKeeper with session timeout {} seconds", session_timeout_seconds);
            }
            return client;
        }
        catch (DB::Exception& ex)
        {
            LOG_ERROR(log, "Failed to connect to ZooKeeper host {}, error {}", host_info_list[i].host, ex.what());
        }
    }
    throwWhenNoHostAvailable(false);
}

}
