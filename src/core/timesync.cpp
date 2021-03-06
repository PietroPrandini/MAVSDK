#include "timesync.h"
#include "log.h"
#include "system_impl.h"

// Partially based on: https://github.com/mavlink/mavros/blob/master/mavros/src/plugins/sys_time.cpp

namespace mavsdk {

Timesync::Timesync(SystemImpl& parent) : _parent(parent)
{
    using namespace std::placeholders; // for `_1`

    _parent.register_mavlink_message_handler(
        MAVLINK_MSG_ID_TIMESYNC, std::bind(&Timesync::process_timesync, this, _1), this);
}

Timesync::~Timesync()
{
    _parent.unregister_all_mavlink_message_handlers(this);
}

void Timesync::do_work()
{
    if (_parent.get_time().elapsed_since_s(_last_time) >= _TIMESYNC_SEND_INTERVAL_S) {
        if (_parent.is_connected()) {
            uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  _parent.get_time().system_time().time_since_epoch())
                                  .count();
            send_timesync(0, now_ns);
        }
        _last_time = _parent.get_time().steady_time();
    }
}

void Timesync::process_timesync(const mavlink_message_t& message)
{
    mavlink_timesync_t timesync;

    mavlink_msg_timesync_decode(&message, &timesync);

    int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         _parent.get_time().system_time().time_since_epoch())
                         .count();

    if (timesync.tc1 == 0) {
        send_timesync(now_ns, timesync.ts1);
    } else if (timesync.tc1 > 0) {
        // Time offset between this system and the remote system is calculated assuming RTT for
        // the timesync packet is roughly equal both ways.
        set_timesync_offset((timesync.tc1 * 2 - (timesync.ts1 + now_ns)) / 2, timesync.ts1);
    }
}

void Timesync::send_timesync(uint64_t tc1, uint64_t ts1)
{
    mavlink_message_t message;

    mavlink_msg_timesync_pack(
        _parent.get_own_system_id(), _parent.get_own_component_id(), &message, tc1, ts1);
    _parent.send_message(message);
}

void Timesync::set_timesync_offset(int64_t offset_ns, uint64_t start_transfer_local_time_ns)
{
    uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          _parent.get_time().system_time().time_since_epoch())
                          .count();

    // Calculate the round trip time (RTT) it took the timesync packet to bounce back to us from
    // remote system
    uint64_t rtt_ns = now_ns - start_transfer_local_time_ns;

    if (rtt_ns < _MAX_RTT_SAMPLE_MS * 1000000ULL) { // Only use samples with low RTT

        // Save time offset for other components to use
        _parent.get_autopilot_time().shift_time_by(std::chrono::nanoseconds(offset_ns));

        // Reset high RTT count after filter update
        _high_rtt_count = 0;
    } else {
        // Increment counter if round trip time is too high for accurate timesync
        _high_rtt_count++;

        if (_high_rtt_count > _MAX_CONS_HIGH_RTT) {
            // Issue a warning to the user if the RTT is constantly high
            LogWarn() << "RTT too high for timesync: " << rtt_ns / 1000000.0 << " ms.";

            // Reset counter
            _high_rtt_count = 0;
        }
    }
}

} // namespace mavsdk
