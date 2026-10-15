#pragma once

#include "gobot/auto_login.hpp"
#include "gobot/crash_watch.hpp"

#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace gobot {

/// Telling the player when something needs a person.
///
/// An unattended run is unattended only until something goes wrong: the
/// character dies, the game falls over, a stop leaves a dialog open. GoC tells
/// its players through Telegram by way of its own server -- the client uploads
/// the game's packets and its account id, and the server decides what to say.
/// GoBot has no server and wants none, so it says it itself: one short message
/// in Vietnamese to a Discord channel of the player's own, through a webhook,
/// and a beep and a Windows notification on this machine.
///
/// A message is built only from fixed sentences and numbers -- a count, a
/// number of minutes, "1/5", an error code of the shape CrashWatch reads. Never
/// a log line, a window title, a path, the account, the character's name, the
/// map or anybody's words: it goes to a server GoBot does not own, and from
/// there to a phone.
///
/// The webhook is a secret -- whoever holds it can post in the channel -- so it
/// is kept encrypted by Windows, outside the program folder, and never written
/// to a log, an ini file or the screen.

// ------------------------------------------------------------------- events

/// One kind of alert. Its key never changes: configs/alerts.ini and the log go
/// by it.
struct AlertEventInfo {
    const char* key;
    /// Needs a person: never dropped from a full queue, never folded into the
    /// "while offline" summary, and sent before the others.
    bool        critical;
    bool        on;             ///< ticked when the file says nothing
    const char* label_vi;       ///< its tick on the Thông báo tab; null for none
};

/// Every event, in the order the tab lists them: death, revived,
/// revive_failed, disconnect, back_in, keeper_gave_up, not_in_world,
/// trash_stuck, chat_stuck, chat_stopped, quest_trouble, party_change -- and
/// test, the tab's Gửi thử, which has no tick.
const std::vector<AlertEventInfo>& alert_events();
/// The event `key` names, or null.
const AlertEventInfo* find_alert_event(const std::string& key);

/// How long a dialog or the chat line may stay in doubt before it is worth a
/// message: long enough for the player at the screen to have dealt with it.
constexpr std::uint64_t kAlertStuckMs = 2 * 60 * 1000;

// ----------------------------------------------------------------- settings

/// configs/alerts.ini. No file is alerts off.
struct AlertSettings {
    bool        enabled = false;
    bool        discord = true;     ///< send to the webhook, when one is saved
    bool        sound = true;       ///< beep on this machine
    bool        toast = true;       ///< a Windows notification on this machine
    /// Put at the head of every message so that two machines can be told apart:
    /// "Máy 1". Typed by the player; GoBot never fills it in.
    std::string label;
    int         merge_s = 60;
    int         max_per_minute = 5;
    int         max_per_hour = 30;
    int         not_in_world_min = 3;
    /// Other players' names in a party message. Off: they are somebody else's.
    bool        party_names = false;
    /// The ticks, by key. A key missing here is at its default.
    std::map<std::string, bool> events;
    /// The file had a webhook= line. It is not read: a webhook in a text file
    /// beside the program goes wherever the folder is copied.
    bool        webhook_in_file = false;

    static constexpr int kMinMerge = 10;
    static constexpr int kMaxMerge = 600;
    static constexpr int kMaxPerMinute = 10;
    static constexpr int kMaxPerHour = 120;
    static constexpr int kMaxAway = 30;
    static constexpr std::size_t kLongestLabel = 32;

    bool event_on(const std::string& key) const;

    /// False, and nothing changed, when the file cannot be read or has no
    /// [Alerts] section. Sections and keys in any case, a BOM in front ignored,
    /// numbers in base ten whatever their leading zeros, and anything out of
    /// range brought into it: the file is edited by hand too.
    bool load(const std::string& path);

    /// Write one key, leaving the rest of the file -- its comments above all --
    /// as it is (IniFile::set_in_place). The file is made, off, if it is not
    /// there.
    static bool save_key(const std::string& path, const std::string& section,
                         const std::string& key, const std::string& value);

    /// A label as it may be used: no '@' or '`' (nothing in a message should
    /// look like a mention or code to Discord), no control characters, trimmed,
    /// at most kLongestLabel characters.
    static std::string clean_label(const std::string& raw);
};

// ------------------------------------------------------------------- texts

/// The sentences, exactly. Each takes only what it may say: numbers and fixed
/// choices -- nothing read off the screen or out of a dialog, except the two
/// party ones, which take a name only when the player allowed it.
namespace alert_text {

std::string death(bool reviving);
std::string revived();
// revive_failed has no sentence here: it is Reviver::out_of_tries_vi(), the
// same words the simple screen shows in red, which say the bot has stopped.
/// `code` only when it has the shape CrashWatch::code_in gives -- a hex or a
/// decimal number -- and is left out otherwise. `coming_back`: the keeper is
/// about to log back in.
std::string disconnect(CrashWatch::Trouble what, const std::string& code, bool coming_back);
std::string back_in(int restart, int max_restarts);
std::string gave_up_restarts(int restarts);
std::string gave_up_not_set_up();
std::string gave_up_not_elevated();
/// A fixed reason for each way a crossing fails, never its detail: that can
/// be the launcher's path, with the Windows user name in it, or the title of
/// some other program's window.
std::string gave_up_crossing(int attempts, AutoLogin::Why why);
std::string not_in_world(int minutes);
std::string in_world_again();
/// `stopped`: a stop has just stopped everything; otherwise it has lasted
/// kAlertStuckMs and everything is held off meanwhile.
std::string trash_stuck(bool stopped);
std::string chat_stuck(bool stopped);
std::string chat_stopped(int failures);
/// `why` is one of the quick quest worker's own fixed phrases.
std::string quest_trouble(int times, const std::string& why);
/// With `with_name` and a name that survives cleaning, the name; otherwise none.
std::string party_joined(const std::string& who, bool with_name);
std::string party_invited(const std::string& who, bool with_name);
std::string test();
std::string offline_summary(int count, const std::string& last_hhmm);
std::string too_many(const std::string& resume_hhmm);

/// Whether `code` looks like what CrashWatch::code_in returns: "0x" and hex
/// digits, or decimal digits.
bool plain_error_code(const std::string& code);

} // namespace alert_text

// ------------------------------------------------------------- time, format

/// Where the hours and minutes in a message come from.
struct AlertClock {
    /// Seconds since 1970 at a moment on the steady clock -- now_ms()'s.
    std::function<std::int64_t(std::uint64_t ms)> wall;
    /// Those seconds as a person's clock shows them.
    std::function<std::tm(std::int64_t s)> local;
    /// The machine's clock and time zone.
    static AlertClock system();
};

/// When something happened: on the steady clock, for the merging and the
/// caps, and on the wall clock, for the time the player reads.
struct AlertTime {
    std::uint64_t ms = 0;
    std::int64_t  wall = 0;
};

/// One message's worth: an event and how many times it happened since the
/// last one like it went, or one of the queue's own messages.
struct AlertBatch {
    enum class Kind { Event, Test, Summary, CapNotice };
    Kind          kind = Kind::Event;
    std::string   key;
    std::string   text;
    bool          critical = false;
    int           n = 0;
    AlertTime     first, last;
    /// A cap notice: when sending goes on, on the steady clock.
    std::uint64_t resume_ms = 0;

    /// What is merged with what: the same event saying the same thing.
    std::string id() const { return key + '\n' + text; }
};

/// "03:10".
std::string alert_hhmm(const std::tm& t);

/// The message as Discord gets it: "GoBot · <label> · HH:MM — <text>", with
/// " (×N, HH:MM–HH:MM)" after it when it stands for several, and the date
/// after the time when it goes out more than an hour late. At most
/// kAlertMaxChars characters, cut with "…".
std::string alert_message(const AlertBatch& b, const std::string& label, std::uint64_t now_ms,
                          const AlertClock& clock);
/// The same without "GoBot" and the label, for a Windows notification, whose
/// title says them.
std::string alert_toast_text(const AlertBatch& b, std::uint64_t now_ms, const AlertClock& clock);
constexpr std::size_t kAlertMaxChars = 1800;

/// The body posted to the webhook: the text, the name it shows under, and no
/// mentions at all -- nothing in a message can ping @everyone. Quotes,
/// backslashes and control characters escaped, the rest as UTF-8.
std::string discord_json(const std::string& content);

/// At most `max` characters (code points) of UTF-8 `s`, the last one "…" when
/// it had to be cut.
std::string cut_utf8(const std::string& s, std::size_t max);

// -------------------------------------------------------------------- queue

/// What waits to be sent, and when it may go. No threads and no clock of its
/// own: every call is told the time, so a test can run an hour in a moment.
///
/// A repeat of an event within the merge window of the last one sent waits,
/// counted, and goes once the window is over -- "×9, 03:10–03:11" -- rather than
/// nine times. On top of that a cap per minute and per hour, critical ones
/// first when they have to wait; a critical one is not held for the hour's
/// cap, only for the minute's. Requests go one at a time, a second apart.
class AlertQueue {
public:
    struct Limits {
        std::uint64_t merge_ms = 60'000;
        int           per_minute = 5;         ///< 0: none
        int           per_hour = 30;          ///< 0: none
        std::uint64_t spacing_ms = 1000;      ///< between two requests
        std::size_t   max_entries = 20;
        /// Waiting this long, a message that needs nobody is folded into one
        /// "while offline" summary. 0: never.
        std::uint64_t stale_ms = 2ull * 3600 * 1000;
    };

    AlertQueue() = default;
    explicit AlertQueue(const Limits& l) : limits_(l) {}
    void set_limits(const Limits& l) { limits_ = l; }
    const Limits& limits() const { return limits_; }

    /// One occurrence. When the queue is full the oldest that needs nobody
    /// goes to make room, and is counted; a critical one is never dropped.
    void push(const std::string& key, const std::string& text, bool critical, AlertTime at);
    /// The tab's Gửi thử: ahead of everything, outside the merging and the
    /// caps, though not ahead of a pause Discord asked for.
    void push_test(const std::string& text, AlertTime at);

    /// The next message that may go at `now`, taken out of the queue; or
    /// nothing, with `wake` set to when there may be one (0: not until
    /// something is pushed).
    std::optional<AlertBatch> take(std::uint64_t now, std::uint64_t& wake);
    /// A request is going out now.
    void requested(std::uint64_t now) { last_request_ = now; }
    /// It was delivered.
    void sent(const AlertBatch& b, std::uint64_t now);
    /// It was not: back in, merged with whatever came since.
    void put_back(AlertBatch b);
    /// Nothing before `ms`: Discord asked for a pause.
    void block_until(std::uint64_t ms) { blocked_until_ = ms; }
    /// Nothing but the test before `ms`: the last attempt failed.
    void retry_at(std::uint64_t ms) { retry_at_ = ms; }

    void clear();
    /// Take out every batch `drop` says.
    void drop_if(const std::function<bool(const AlertBatch&)>& drop);
    std::size_t size() const { return pending_.size() + (test_ ? 1 : 0); }
    int dropped() const { return dropped_; }
    /// The last take() waited for a cap.
    bool capped() const { return capped_; }

private:
    void insert(AlertBatch b);
    void fold_stale(std::uint64_t now);
    void prune(std::uint64_t now);

    Limits                               limits_;
    std::vector<AlertBatch>              pending_;
    std::optional<AlertBatch>            test_;
    std::map<std::string, std::uint64_t> last_sent_;    ///< by AlertBatch::id()
    struct Sent { std::uint64_t at; bool hourly; };
    std::deque<Sent>                     sent_;         ///< the last hour's
    std::optional<std::uint64_t>         last_request_;
    std::optional<std::uint64_t>         last_notice_;
    std::uint64_t                        blocked_until_ = 0;
    std::uint64_t                        retry_at_ = 0;
    int                                  dropped_ = 0;
    bool                                 capped_ = false;
};

// ------------------------------------------------------------------ sending

/// What a channel said to one message.
struct ChannelReply {
    int           http_status = 0;      ///< 0: no answer
    std::uint64_t retry_after_ms = 0;   ///< with 429
    unsigned long net_error = 0;        ///< with no answer: WinHTTP's error
};

/// Somewhere a message goes. Discord's webhook now; a Zalo bot may be another.
class AlertChannel {
public:
    virtual ~AlertChannel() = default;
    /// Post one message's JSON body. Blocks for as long as the request takes.
    virtual ChannelReply send(const std::string& json) = 0;
    /// From another thread: end a send under way at once. The channel is not
    /// used again afterwards.
    virtual void cancel() {}
};

struct WebhookUrl;

/// The webhook, through WinHTTP, as GoC's own GhostCore posts. The address
/// lives in here and nowhere else, and is zeroed when this goes.
std::unique_ptr<AlertChannel> make_discord_channel(const WebhookUrl& url);

/// The beep and the notification on this machine. The GUI's.
class LocalNotifier {
public:
    virtual ~LocalNotifier() = default;
    virtual void beep() = 0;
    virtual void toast(const std::string& title, const std::string& text) = 0;
};

// -------------------------------------------------------------- the webhook

/// A Discord webhook's address, taken apart.
///
/// Only https://discord.com/api/webhooks/<id>/<token>, as Discord's Copy
/// Webhook URL gives it -- or the same on discordapp.com, canary.discord.com or
/// ptb.discord.com, with /v<n> after /api, or ?thread_id=<n> after it. Anything
/// else is refused: the address is all there is to say where a message goes,
/// and one that only looked like Discord's would send the player's alerts to
/// somebody else.
struct WebhookUrl {
    std::string host;
    std::string path;       ///< from /api on, with the query
    std::string id;
    std::string token;

    static bool parse(const std::string& text, WebhookUrl& out);
    /// "…abcd": the token's last four characters, and nothing else of it.
    std::string masked() const;
    /// Zero every copy this holds.
    void wipe();
};

/// What the tab says to an address that is not a webhook.
extern const char* const kWebhookRefused;

/// Zero a string's whole buffer, then empty it.
void wipe_string(std::string& s);

/// The webhook, encrypted by Windows (DPAPI) for this Windows user on this
/// machine, in a file of its own.
///
/// Outside the program folder by default -- %LOCALAPPDATA%\GoBot -- so that
/// neither a build, a copied GoBot folder nor a package can carry it; and a
/// file that does reach another user or machine does not decrypt there.
class SecretStore {
public:
    /// `path` in UTF-8.
    explicit SecretStore(std::string path) : path_(std::move(path)) {}
    /// %LOCALAPPDATA%\GoBot\alerts_webhook.bin.
    static std::string default_path();
    const std::string& path() const { return path_; }

    bool exists() const;
    /// Written beside the file and moved over it, so that a failed write
    /// leaves the old one.
    bool save(const std::string& secret) const;
    /// False when there is no file or it will not decrypt -- another user's,
    /// another machine's, or damaged.
    bool load(std::string& secret) const;
    /// Delete the file. True when it is gone, or was never there.
    bool clear() const;

private:
    std::string path_;
};

// ------------------------------------------------------------------ service

/// What the tab shows of the service.
struct AlertStatus {
    bool          enabled = false;
    bool          running = false;      ///< the worker is up
    bool          webhook = false;      ///< one is saved and was read
    std::string   masked;               ///< "…abcd" while there is one
    bool          unreadable = false;   ///< the saved one would not decrypt
    bool          busy = false;         ///< a save, a removal or a read waits
    enum class Discord { None, Ready, Sent, Waiting, Retrying, Disabled };
    Discord       discord = Discord::None;
    int           code = 0;             ///< the reply that stopped sending
    std::int64_t  sent_wall = 0;        ///< when the last message went
    std::int64_t  resume_wall = 0;      ///< when sending goes on, waiting or retrying
    bool          capped = false;       ///< holding messages back for the caps
    std::size_t   queued = 0;
    int           dropped = 0;
    std::uint64_t test_ready_ms = 0;    ///< Gửi thử may be pressed again from then
    /// The last few things that happened, in Vietnamese, with their times.
    std::vector<std::string> lines;
    /// Bumped with every new line.
    unsigned      seq = 0;
    /// Something the player should see now, wherever they are on the screen:
    /// Discord refused a message, or the webhook. Bumped with every new one.
    std::string   note;
    unsigned      note_seq = 0;
};

/// The alerts: one worker thread for the network and the stored webhook, and
/// the beep and notification on whichever thread calls step_local().
///
/// raise() and send_test() only queue, under a short lock, and wake the
/// worker; they never wait for it, so no part of the bot is held up by a
/// network that is down or a request that hangs. The beep and notification
/// do not wait for the network either: step_local() is called from the GUI's
/// tick, and a request under way on the worker is no reason to be late.
///
/// Replies: 2xx sent. 429 waits as long as Discord asks, a minute at most, and
/// sends the same message again. 401, 403 and 404 stop every send until a new
/// webhook is saved or a Gửi thử goes through -- Discord bans an address that
/// keeps posting to a webhook that is gone -- and the next GoBot opened knows
/// it too: the refusal's number is kept beside the stored webhook, in
/// <secret>.refused, and read back with it. 5xx and no answer keep the
/// message and try again after 10 s, 30 s, 1 min, 2 min, then every 5 min.
/// Anything else drops that message.
class AlertService {
public:
    using ChannelFactory = std::function<std::unique_ptr<AlertChannel>(const WebhookUrl&)>;

    /// `secret_path`: where the webhook is kept (SecretStore).
    explicit AlertService(std::string secret_path);
    ~AlertService();

    AlertService(const AlertService&) = delete;
    AlertService& operator=(const AlertService&) = delete;

    /// Applied at once. Turning alerts, Discord or an event off drops what
    /// waits for it.
    void set_settings(const AlertSettings& s);
    AlertSettings settings() const;
    void set_notifier(LocalNotifier* n);
    void set_clock(AlertClock c);
    /// What a saved webhook is sent through. Discord's own unless a test's.
    void set_channel_factory(ChannelFactory f);
    /// A channel to use now, as if a webhook had been saved; null for none.
    void set_channel(std::unique_ptr<AlertChannel> c);

    /// The worker. Started by whoever wants it -- nothing here starts it --
    /// so that a test can step the service by hand instead.
    void start();
    /// Ends a send under way, joins the worker -- within a few seconds,
    /// whatever the network is doing -- and drops the channel, and the
    /// webhook's address with it.
    void stop();
    bool running() const;

    /// One event. False when it is not sent anywhere: alerts off, the event
    /// unticked, or a key no event has.
    bool raise(const std::string& event, const std::string& text_vi, std::uint64_t now);
    /// The tab's Gửi thử: to Discord when a webhook is saved, whatever is
    /// ticked, and here. False within 30 s of the last.
    bool send_test(std::uint64_t now);
    static constexpr std::uint64_t kTestGapMs = 30'000;

    /// Encrypt and keep a webhook, and send through it from then on; done on
    /// the worker. The text is wiped once it has been used.
    void save_webhook(std::string url);
    /// Read the saved one, on the worker.
    void load_webhook();
    /// Delete it, on the worker; nothing goes to Discord until another.
    void clear_webhook();

    /// What the worker does each time round. Public so that a test can be the
    /// worker: one request at most. Returns when to come back (0: not until
    /// woken).
    std::uint64_t step_discord(std::uint64_t now);
    /// The webhook jobs waiting.
    void step_jobs();
    /// Beeps and notifications due at `now`. Returns when the next is.
    std::uint64_t step_local(std::uint64_t now);

    AlertStatus status() const;

private:
    void run();
    void say(const std::string& vi, std::uint64_t now);
    AlertQueue::Limits discord_limits() const;
    AlertQueue::Limits local_limits() const;
    void drop_unwanted();
    std::string title() const;

    mutable std::mutex      mutex_;
    std::condition_variable wake_;
    std::thread             worker_;
    bool                    running_ = false;
    bool                    stopping_ = false;
    std::uint64_t           work_ = 0;          ///< bumped whenever there is something new

    AlertSettings           settings_;
    AlertClock              clock_ = AlertClock::system();
    LocalNotifier*          notifier_ = nullptr;
    SecretStore             store_;
    ChannelFactory          factory_;
    std::shared_ptr<AlertChannel> channel_;
    std::shared_ptr<AlertChannel> in_flight_;   ///< the channel a send is under way on
    std::string             masked_;
    bool                    unreadable_ = false;

    struct Job {
        enum class Kind { Save, Load, Clear } kind;
        std::string text;
    };
    std::vector<Job>        jobs_;
    std::size_t             jobs_open_ = 0;     ///< asked for and not yet done

    AlertQueue              discord_;
    AlertQueue              local_;
    bool                    disabled_ = false;
    int                     disabled_code_ = 0;
    int                     fail_streak_ = 0;
    AlertStatus::Discord    state_ = AlertStatus::Discord::Ready;
    std::int64_t            sent_wall_ = 0;
    std::int64_t            resume_wall_ = 0;
    std::optional<std::uint64_t> last_beep_;
    std::optional<std::uint64_t> last_test_;
    std::deque<std::string> lines_;
    unsigned                seq_ = 0;
    std::string             note_;
    unsigned                note_seq_ = 0;
};

// -------------------------------------------------------------------- watch

/// Something that has to have gone on for a while before it is worth an
/// alert: due() is true once, the first time `on` has held for the whole of
/// `ms`, and only again after it has stopped.
class AlertWatch {
public:
    explicit AlertWatch(std::uint64_t ms) : ms_(ms) {}
    bool due(bool on, std::uint64_t now);
    /// It has been due since `on` last began.
    bool told() const { return told_; }
    void reset() { active_ = false; told_ = false; }
    void set_ms(std::uint64_t ms) { ms_ = ms; }

private:
    std::uint64_t ms_;
    std::uint64_t since_ = 0;
    bool          active_ = false;
    bool          told_ = false;
};

/// The character out of the world, as the HP/MP reader sees it: during a run,
/// the bars gone for `ms` -- having been on screen this run, so that a reader
/// that was never set up says nothing -- and then back. Not while the keeper
/// is crossing back in, which says what it is doing itself.
class AwayWatch {
public:
    enum class Say { Nothing, Away, Back };
    explicit AwayWatch(std::uint64_t ms) : watch_(ms) {}
    void set_ms(std::uint64_t ms) { watch_.set_ms(ms); }
    /// `hp_pct` is the reader's: -1 while the bars are not on screen.
    Say step(bool running, bool reading, bool crossing, float hp_pct, std::uint64_t now);

private:
    AlertWatch watch_;
    bool       seen_ = false;
};

/// A death told by the HP figure alone, for when nothing else will tell of
/// it: read as 0 of something for `ms`, once a death. Only HP read above 0
/// makes the next one news; a figure that did not read neither counts nor
/// clears.
class DeathByHp {
public:
    explicit DeathByHp(std::uint64_t ms = 3000) : ms_(ms) {}
    bool step(bool watching, int hp, int hp_max, std::uint64_t now);

private:
    std::uint64_t                ms_;
    std::optional<std::uint64_t> since_;
    bool                         told_ = false;
};

} // namespace gobot
