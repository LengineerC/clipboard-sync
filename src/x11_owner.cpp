#include <xcb/xcb.h>
#include <poll.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

#include "incr.hpp"

namespace {
using clipsync::IncrTransfers;
template <typename T>
using Reply = std::unique_ptr<T, decltype(&std::free)>;

bool checked(xcb_connection_t* connection, xcb_void_cookie_t cookie) {
    Reply<xcb_generic_error_t> error(xcb_request_check(connection, cookie), std::free);
    return !error && !xcb_connection_has_error(connection);
}
xcb_atom_t atom(xcb_connection_t* connection, const char* name) {
    Reply<xcb_intern_atom_reply_t> reply(
        xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 0, std::strlen(name), name), nullptr),
        std::free);
    return reply ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
}
int serve(const char* mime, const std::vector<unsigned char>& data) {
    int screen_number = 0;
    std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> connection(
        xcb_connect(nullptr, &screen_number), xcb_disconnect);
    auto* c = connection.get();
    if (!c || xcb_connection_has_error(c)) return 1;
    auto screens = xcb_setup_roots_iterator(xcb_get_setup(c));
    for (int i = 0; i < screen_number && screens.rem; ++i) xcb_screen_next(&screens);
    if (!screens.rem) return 1;
    const auto window = xcb_generate_id(c);
    const std::uint32_t events = XCB_EVENT_MASK_PROPERTY_CHANGE;
    if (!checked(c, xcb_create_window_checked(c, XCB_COPY_FROM_PARENT, window, screens.data->root,
                                              0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                                              XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &events))) return 1;
    const auto clipboard = atom(c, "CLIPBOARD");
    const auto targets = atom(c, "TARGETS");
    const auto timestamp = atom(c, "TIMESTAMP");
    const auto incr = atom(c, "INCR");
    const auto target = atom(c, mime);
    if (!clipboard || !targets || !timestamp || !incr || !target) return 1;

    // Obtain a server timestamp before claiming the selection (ICCCM 2.1).
    if (!checked(c, xcb_change_property_checked(c, XCB_PROP_MODE_REPLACE, window, timestamp,
                                                XCB_ATOM_INTEGER, 32, 0, nullptr))) return 1;
    xcb_timestamp_t acquired = XCB_CURRENT_TIME;
    while (!acquired) {
        Reply<xcb_generic_event_t> event(xcb_wait_for_event(c), std::free);
        if (!event) return 1;
        if ((event->response_type & 0x7f) == XCB_PROPERTY_NOTIFY) {
            const auto* property = reinterpret_cast<xcb_property_notify_event_t*>(event.get());
            if (property->window == window && property->atom == timestamp) acquired = property->time;
        }
    }
    if (!checked(c, xcb_set_selection_owner_checked(c, window, clipboard, acquired))) return 1;
    Reply<xcb_get_selection_owner_reply_t> owner(
        xcb_get_selection_owner_reply(c, xcb_get_selection_owner(c, clipboard), nullptr), std::free);
    if (!owner || owner->owner != window) return 1;
    const auto max_bytes = static_cast<std::size_t>(xcb_get_maximum_request_length(c)) * 4;
    if (max_bytes <= sizeof(xcb_change_property_request_t) + 4) return 1;
    const auto chunk_size = std::min<std::size_t>(65536, max_bytes - sizeof(xcb_change_property_request_t) - 4);
    IncrTransfers transfers(data.size(), chunk_size);
    bool owns_selection = true;
    auto unwatch = [&](xcb_window_t requestor) {
        if (!transfers.watches(requestor) && requestor != window) {
            const std::uint32_t mask = 0;
            (void)checked(c, xcb_change_window_attributes_checked(c, requestor, XCB_CW_EVENT_MASK, &mask));
        }
    };
    // Keep running in the foreground. Acknowledge only after initialization and
    // ownership verification, retaining the actual data-serving PID.
    std::cout << "READY\n" << std::flush;
    if (!std::cout) return 1;
    for (;;) {
        // Drain XCB's queue before polling its socket: checked requests may have
        // already read the next event from the socket into this queue.
        for (;;) {
            Reply<xcb_generic_event_t> event(xcb_poll_for_event(c), std::free);
            if (!event) break;
            switch (event->response_type & 0x7f) {
                case XCB_SELECTION_REQUEST: {
                    const auto* request = reinterpret_cast<xcb_selection_request_event_t*>(event.get());
                    const auto property = request->property ? request->property : request->target;
                    const IncrTransfers::Key key{request->requestor, property};
                    bool accepted = false;
                    bool started = false;
                    const bool recent = request->time == XCB_CURRENT_TIME ||
                        static_cast<std::int32_t>(request->time - acquired) >= 0;
                    if (owns_selection && recent && request->selection == clipboard && request->owner == window &&
                        property && !transfers.contains(key)) {
                        if (request->target == targets) {
                            const xcb_atom_t types[] = {targets, timestamp, target};
                            accepted = checked(c, xcb_change_property_checked(
                                c, XCB_PROP_MODE_REPLACE, request->requestor, property, XCB_ATOM_ATOM, 32, 3, types));
                        } else if (request->target == timestamp) {
                            accepted = checked(c, xcb_change_property_checked(
                                c, XCB_PROP_MODE_REPLACE, request->requestor, property, XCB_ATOM_INTEGER, 32, 1, &acquired));
                        } else if (request->target == target) {
                            if (data.size() <= chunk_size) {
                                accepted = checked(c, xcb_change_property_checked(
                                    c, XCB_PROP_MODE_REPLACE, request->requestor, property, target, 8,
                                    data.size(), data.data()));
                            } else if (transfers.begin(key, IncrTransfers::Clock::now())) {
                                started = true;
                                const std::uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
                                const std::uint32_t size = data.size();
                                accepted = checked(c, xcb_change_window_attributes_checked(
                                    c, request->requestor, XCB_CW_EVENT_MASK, &mask)) &&
                                    checked(c, xcb_change_property_checked(c, XCB_PROP_MODE_REPLACE,
                                        request->requestor, property, incr, 32, 1, &size));
                            }
                        }
                    }
                    union {
                        xcb_selection_notify_event_t event;
                        char bytes[32];
                    } response{};
                    auto& notify = response.event;
                    notify.response_type = XCB_SELECTION_NOTIFY;
                    notify.time = request->time;
                    notify.requestor = request->requestor;
                    notify.selection = request->selection;
                    notify.target = request->target;
                    notify.property = accepted ? property : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
                    const bool notified = checked(c, xcb_send_event_checked(
                        c, 0, request->requestor, XCB_EVENT_MASK_NO_EVENT, response.bytes));
                    if (started && (!accepted || !notified)) {
                        transfers.erase(key);
                        unwatch(request->requestor);
                    }
                    break;
                }
                case XCB_PROPERTY_NOTIFY: {
                    const auto* property = reinterpret_cast<xcb_property_notify_event_t*>(event.get());
                    if (property->state != XCB_PROPERTY_DELETE) break;
                    const IncrTransfers::Key key{property->window, property->atom};
                    const auto chunk = transfers.deleted(key, IncrTransfers::Clock::now());
                    if (!chunk) break;
                    if (!checked(c, xcb_change_property_checked(c, XCB_PROP_MODE_REPLACE, property->window,
                        property->atom, target, 8, chunk->size, data.data() + chunk->offset))) transfers.erase(key);
                    unwatch(property->window);
                    break;
                }
                case XCB_DESTROY_NOTIFY:
                    transfers.destroyed(reinterpret_cast<xcb_destroy_notify_event_t*>(event.get())->window);
                    break;
                case XCB_SELECTION_CLEAR:
                    // Finish transfers already accepted, but stop accepting new requests.
                    owns_selection = false;
                    break;
            }
        }
        if (xcb_connection_has_error(c)) return 1;
        for (const auto& key : transfers.expire(IncrTransfers::Clock::now())) {
            unwatch(key.first);
            std::cerr << "X11 INCR 接收方超时，释放该传输\n";
        }
        if (!owns_selection && transfers.empty()) return 0;
        xcb_flush(c);
        pollfd fd{xcb_get_file_descriptor(c), POLLIN, 0};
        if (poll(&fd, 1, 250) < 0 && errno != EINTR) return 1;
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    char* end = nullptr;
    const long parent = std::strtol(argv[2], &end, 10);
    if (!end || *end || parent <= 0 || parent > std::numeric_limits<pid_t>::max()) return 2;
    // Avoid orphan providers when the daemon is terminated outside systemd.
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent) return 1;
    std::vector<unsigned char> data;
    unsigned char buffer[65536];
    for (;;) {
        const auto count = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count > 0) {
            if (data.size() + count > std::numeric_limits<std::uint32_t>::max()) return 1;
            data.insert(data.end(), buffer, buffer + count);
        } else if (!count) {
            break;
        } else if (errno != EINTR) {
            return 1;
        }
    }
    // stdin is a memfd supplied by the parent; release it after loading the
    // payload so it does not retain a second copy for the owner's lifetime.
    close(STDIN_FILENO);
    return serve(argv[1], data);
}
