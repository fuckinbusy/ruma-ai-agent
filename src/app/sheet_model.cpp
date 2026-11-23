#include "app/sheet_model.hpp"

#include "app/details.hpp"
#include "app/keycheck.hpp"
#include "app/space.hpp"

#include <algorithm>
#include <charconv>
#include <format>

namespace app::sheet {
namespace {

// The budget's "Go on?" (its Go on and Stop buttons): the task waits for the user, it has not ended.
auto go_on(const events::Actions& a) -> bool {
    return std::ranges::any_of(a.actions, [](const events::Action& x) { return x.id == "continue"; });
}

// A task stopped at "Go on?" (phase 10, final review): behind the handle it needs a decision.
auto waits_go_on(const TaskState& t) -> bool {
    return std::ranges::any_of(t.actions, [](const events::Action& x) { return x.id == "continue"; });
}

// Its question as the agent put it: the answer's last paragraph ("This has taken many steps. Go on?").
auto last_paragraph(std::string_view text) -> std::string {
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    const auto at = text.rfind("\n\n");
    return std::string{at == std::string_view::npos ? text : text.substr(at + 2)};
}

// An answer's first sentence: the headline of its preview (screen 14).
auto first_sentence(std::string_view text) -> std::string_view {
    const auto start = text.find_first_not_of(" \n\r\t#*");
    if (start == std::string_view::npos) {
        return {};
    }
    text.remove_prefix(start);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\n' || ((c == '.' || c == '!' || c == '?') && (i + 1 == text.size() || text[i + 1] == ' '))) {
            return text.substr(0, c == '\n' ? i : i + 1);
        }
    }
    return text.substr(0, std::min<std::size_t>(text.size(), 160));
}

void behind(Model& m);
auto bring_front(Model& m, std::uint64_t task) -> bool;

// The work is over: no step stays running or waiting — its spinner and its sweep would turn
// forever on a screen where nothing happens (no bar may move then). A running step ends as `ran`,
// a waiting one never ran.
void settle_steps(std::vector<events::Step>& steps, events::StepState ran) {
    for (auto& s : steps) {
        if (s.state == events::StepState::Run) {
            s.state = ran;
        } else if (s.state == events::StepState::Wait) {
            s.state = events::StepState::Skipped;
        }
    }
}

auto is_continuation(char c) noexcept -> bool {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// Byte offset of the character before / after `pos`.
auto prev_char(const std::string& s, std::size_t pos) noexcept -> std::size_t {
    if (pos == 0) {
        return 0;
    }
    --pos;
    while (pos > 0 && is_continuation(s[pos])) {
        --pos;
    }
    return pos;
}

auto next_char(const std::string& s, std::size_t pos) noexcept -> std::size_t {
    if (pos >= s.size()) {
        return s.size();
    }
    ++pos;
    while (pos < s.size() && is_continuation(s[pos])) {
        ++pos;
    }
    return pos;
}

auto is_space(char c) noexcept -> bool {
    return c == ' ';
}

auto trimmed(std::string_view s) -> std::string {
    while (!s.empty() && is_space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_space(s.back())) {
        s.remove_suffix(1);
    }
    return std::string{s};
}

// What a task shows of its work: gone when a new one starts in its place.
void clear_work(TaskState& t) {
    t.steps.clear();
    t.summary.reset();
    t.progress = -1;
    t.quick.reset();
    t.quick_more = false;
    t.report.reset();
    t.plan.reset();
    t.checklist.reset();
    t.remembered.clear();
    t.looked.clear();
    t.tabs.clear();
    t.spent.reset();
    t.suggestions.clear();
    t.word.reset();
    t.phrase.reset();
    t.text_translation.reset();
    t.pictures.clear();
    t.refusals.clear();
    t.created.clear();
    t.places.clear();
}

// The question card's reply (phase 12 § 4); the card goes.
auto replied(Model& m, std::vector<std::string> chosen, std::string other, bool answered) -> events::Command {
    events::Reply r{.task = m.task, .id = m.query->id, .chosen = std::move(chosen), .other = std::move(other), .answered = answered};
    m.query.reset();
    m.picks.clear();
    m.answering_other = false;
    return r;
}

// A new task's id: past every id given (one brought back from the background keeps its own).
auto next_task(Model& m) -> std::uint64_t {
    m.last_task = std::max(m.last_task, m.task) + 1;
    return m.last_task;
}

// The turn in front becomes the session's newest earlier turn.
void archive(Model& m) {
    const double end = m.ended_at >= 0 ? m.ended_at : m.now;
    m.session.insert(m.session.begin(), Turn{.task = m.task,
                                             .question = m.question,
                                             .time = m.time,
                                             .answer = m.answer,
                                             .steps = m.steps,
                                             .data = std::move(m.data),
                                             .activity = std::move(m.activity_log),
                                             .pictures = std::move(m.pictures),
                                             .seconds = std::max(0.0, end - m.started_at),
                                             .collapsed = true,
                                             .checklist = std::move(m.checklist)});
    m.checklist.reset();
}

// Another set in the panel: its own rows, no filter (the filter was the other set's).
void activate(Model& m, std::uint32_t id) {
    if (m.active_set != id) {
        m.filter = {};
        m.filter_focus = false;
        m.private_focus = false;
        m.doc_page = 0;
    }
    m.active_set = id;
}

// A data set of a task: a new one, its next rows, or the same id again whole.
// A set's pictures get their ids as the model takes it (the window's textures are found by them):
// a read picture's, the converted files' thumbnails (phase 10).
void give_ids(Model& model, events::DataSet& set) {
    if (set.text_read) {
        set.text_read->picture.id = ++model.pictures_taken;
    }
    if (set.converted) {
        for (auto& row : set.converted->rows) {
            row.thumb.id = ++model.pictures_taken;
        }
    }
}

void take_data(Model& model, TaskState& t, const events::Data& d) {
    auto it = std::ranges::find(model.sets, d.set.id, &events::DataSet::id);
    if (it == model.sets.end()) {
        model.sets.push_back(d.set);
        give_ids(model, model.sets.back());
        t.data.push_back(d.set.id);
        if (&t == &model) {
            activate(model, d.set.id); // the newest in the panel
        }
        return;
    }
    if (!d.append) {
        *it = d.set;
        give_ids(model, *it);
        return;
    }
    if (d.set.pages && it->pages) { // a document's next pages: in their order, a page read again replaced (phase 10)
        auto& read = it->pages->read;
        for (const auto& p : d.set.pages->read) {
            const auto at = std::ranges::lower_bound(read, p.page, {}, &events::PageText::page);
            if (at != read.end() && at->page == p.page) {
                *at = p;
            } else {
                read.insert(at, p);
            }
        }
        it->pages->mapped = d.set.pages->mapped;
        it->pages->note = d.set.pages->note;
        const bool scans = std::ranges::any_of(read, &events::PageText::scan);
        it->footer = std::format("{} pages \xC2\xB7 {}", it->pages->count, scans ? "text + scans" : "text");
        return;
    }
    auto& rows = it->rows.cells;
    rows.insert(rows.end(), d.set.rows.cells.begin(), d.set.rows.cells.end());
    auto& group = it->rows.group;
    group.insert(group.end(), d.set.rows.group.begin(), d.set.rows.group.end());
    auto& faded = it->rows.faded;
    faded.insert(faded.end(), d.set.rows.faded.begin(), d.set.rows.faded.end());
    it->complete = d.set.complete;
    if (!d.set.footer.empty()) {
        it->footer = d.set.footer;
    }
    if (!d.set.snapshot.empty()) {
        it->snapshot = d.set.snapshot;
    }
}

// The data sets no task holds any more go: the front's, a background task's and the session's stay.
void forget_sets(Model& m) {
    const auto holds = [](const std::vector<std::uint32_t>& data, std::uint32_t id) { return std::ranges::find(data, id) != data.end(); };
    const auto held = [&](std::uint32_t id) {
        return holds(m.data, id) || std::ranges::any_of(m.background, [&](const TaskState& t) { return holds(t.data, id); }) ||
               std::ranges::any_of(m.session, [&](const Turn& t) { return holds(t.data, id); });
    };
    std::erase_if(m.sets, [&](const events::DataSet& s) { return !held(s.id); });
}

// The agent began a new conversation for the task in front: the earlier turns go, and with
// them the data only they had.
void end_session(Model& m) {
    m.session.clear();
    m.context = 0; // a new conversation: its history empty
    forget_sets(m);
    if (std::ranges::find(m.data, m.active_set) == m.data.end()) {
        activate(m, m.data.empty() ? 0 : m.data.back());
    }
}

void remember_last(Model& m, const std::string& question) {
    if (m.recent.empty() || m.recent.back() != question) {
        m.recent.push_back(question);
    }
    m.recall = 0;
}

// Up and Down: the questions asked before, newest first; past the newest, an empty field.
void recall(Model& m, bool older) {
    if (older) {
        if (m.recall >= m.recent.size()) {
            return;
        }
        ++m.recall;
    } else {
        if (m.recall == 0) {
            return;
        }
        --m.recall;
    }
    m.input.text = m.recall == 0 ? std::string{} : m.recent[m.recent.size() - m.recall];
    m.input.caret = m.input.text.size();
    m.input.anchor = Edit::kNoAnchor; // a recalled text: no selection of the old one
}

// The task an event belongs to: the one in front, or one in the background; none for a task the
// user already stopped or replaced.
auto owner(Model& m, std::uint64_t task) -> TaskState* {
    if (task == m.task) {
        return &m;
    }
    for (auto& t : m.background) {
        if (t.task == task) {
            return &t;
        }
    }
    return nullptr;
}

} // namespace

auto selection(const Edit& e) -> std::pair<std::size_t, std::size_t> {
    if (e.anchor == Edit::kNoAnchor) {
        return {e.caret, e.caret};
    }
    const auto anchor = std::min(e.anchor, e.text.size());
    return {std::min(anchor, e.caret), std::max(anchor, e.caret)};
}

auto erase_selection(Edit& e) -> bool {
    const auto [from, to] = selection(e);
    e.anchor = Edit::kNoAnchor;
    if (from == to) {
        return false;
    }
    e.text.erase(from, to - from);
    e.caret = from;
    return true;
}

void extend(Edit& e, bool shift, void (*step)(Edit&)) {
    const auto at = e.caret;
    step(e);
    if (!shift) {
        e.anchor = Edit::kNoAnchor;
        return;
    }
    if (e.anchor == Edit::kNoAnchor) {
        e.anchor = at;
    }
    if (e.anchor == e.caret) {
        e.anchor = Edit::kNoAnchor; // back where it began: nothing selected
    }
}

namespace {

// The field the keys go to; none in the key's mode (a secret is not copied).
auto focused_edit(Model& m) -> Edit* {
    if (m.key_mode) {
        return nullptr;
    }
    if (m.menu.open) {
        return &m.menu.query;
    }
    return m.private_focus ? &m.private_add : m.filter_focus ? &m.filter : &m.input;
}

} // namespace

auto field_selected(const Model& m) -> std::string_view {
    const auto* e = focused_edit(const_cast<Model&>(m));
    if (e == nullptr) {
        return {};
    }
    const auto [from, to] = selection(*e);
    return std::string_view{e->text}.substr(from, to - from);
}

auto cut_field(Model& m) -> std::string {
    std::string cut{field_selected(m)};
    if (auto* e = focused_edit(m); e != nullptr && !cut.empty()) {
        (void)erase_selection(*e);
    }
    return cut;
}

void select_all_field(Model& m) {
    if (auto* e = focused_edit(m); e != nullptr && !e->text.empty()) {
        e->anchor = 0;
        e->caret = e->text.size();
    }
}

void insert(Edit& e, std::string_view text, bool lines) {
    std::string clean;
    clean.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            continue; // CRLF: one, from the LF
        }
        if (c == '\r' || c == '\n') {
            clean.push_back(lines ? '\n' : ' ');
        } else if (c == '\t') {
            clean.push_back(' ');
        } else if (static_cast<unsigned char>(c) >= 0x20 || c < 0) {
            clean.push_back(c);
        }
    }
    (void)erase_selection(e); // typed over a selection: it goes
    e.text.insert(e.caret, clean);
    e.caret += clean.size();
}

void backspace(Edit& e) {
    if (erase_selection(e)) {
        return;
    }
    const std::size_t from = prev_char(e.text, e.caret);
    e.text.erase(from, e.caret - from);
    e.caret = from;
}

void del(Edit& e) {
    if (erase_selection(e)) {
        return;
    }
    const std::size_t to = next_char(e.text, e.caret);
    e.text.erase(e.caret, to - e.caret);
}

void left(Edit& e) {
    e.caret = prev_char(e.text, e.caret);
}

void right(Edit& e) {
    e.caret = next_char(e.text, e.caret);
}

void home(Edit& e) {
    e.caret = 0;
}

void end(Edit& e) {
    e.caret = e.text.size();
}

void word_left(Edit& e) {
    std::size_t pos = e.caret;
    while (pos > 0 && is_space(e.text[pos - 1])) {
        --pos;
    }
    while (pos > 0 && !is_space(e.text[pos - 1])) {
        --pos;
    }
    e.caret = pos;
}

void word_right(Edit& e) {
    std::size_t pos = e.caret;
    while (pos < e.text.size() && !is_space(e.text[pos])) {
        ++pos;
    }
    while (pos < e.text.size() && is_space(e.text[pos])) {
        ++pos;
    }
    e.caret = pos;
}

void backspace_word(Edit& e) {
    if (erase_selection(e)) {
        return;
    }
    const std::size_t to = e.caret;
    word_left(e);
    e.text.erase(e.caret, to - e.caret);
}

auto working(const TaskState& t) -> bool {
    return t.running;
}

auto busy(const Model& m) -> bool {
    return working(m) || std::ranges::any_of(m.background, [](const TaskState& t) { return working(t); });
}

auto effective(const Model& m) -> ModelChoice {
    return m.next_only.value_or(m.choice);
}

// A picture in the input and a model that does not see: the request goes to one of its provider that does.
auto images_elsewhere(const Model& m) -> bool {
    const auto& chosen = model_of(effective(m));
    const auto* sees = ai::catalog::beside(chosen).vision;
    return chosen.vision != ai::catalog::Tri::Yes && sees != nullptr &&
           std::ranges::any_of(m.attachments, [](const Attachment& a) { return a.image.has_value() && a.refused.empty(); });
}

auto sent_with(const Model& m) -> ModelChoice {
    ModelChoice c = effective(m);
    if (images_elsewhere(m)) {
        c.model = ai::catalog::beside(model_of(c)).vision->id;
    }
    return c;
}

namespace {

// The quick changes change the change for the next request when there is one, else the setting
// (which the shell saves: Reaction::choose).
auto changed(Model& m, ModelChoice c) -> Reaction {
    Reaction r;
    if (m.next_only) {
        m.next_only = c;
    } else {
        m.choice = c;
        r.choose = c;
    }
    return r;
}

void hint(Model& m, std::string text, double now, bool limit = false) {
    m.choice_hint = {.text = std::move(text), .at = now, .limit = limit};
}

// A quick change while a change for the next request waits changes that one: the hint says so.
auto once_said(const Model& m) -> std::string_view {
    return m.next_only ? std::string_view{" (next request)"} : std::string_view{};
}

} // namespace

auto set_choice(Model& m, ModelChoice choice, bool next_only, double now) -> Reaction {
    (void)now;
    if (next_only) {
        m.next_only = choice;
        return {};
    }
    m.next_only.reset();
    if (choice.model != m.choice.model) {
        m.previous_model = m.choice.model;
    }
    m.choice = choice;
    Reaction r;
    r.choose = choice;
    return r;
}

auto toggle_model(Model& m, double now) -> Reaction {
    ModelChoice c = effective(m);
    const std::string_view to_id = !m.previous_model.empty() && m.previous_model != c.model ? m.previous_model : next_model(c.model);
    if (!m.next_only) {
        m.previous_model = c.model;
    }
    c.model = to_id;
    const auto& to = model_of(c);
    if (working(m)) {
        hint(m, std::format("{} from the next request", to.short_name), now);
    } else {
        hint(m, std::format("{}{}", to.full_name, once_said(m)), now);
    }
    return changed(m, c);
}

auto step_level(Model& m, int delta, double now) -> Reaction {
    const ModelChoice was = effective(m);
    ModelChoice c = was;
    c.level = std::clamp(was.level + delta, 0, 3);
    if (c.level == was.level) { // past Off or Max: nothing, the hint says why
        hint(m, std::format("Thinking {} \xE2\x80\x94 the limit", thinking_name(was.level)), now, true);
        return {};
    }
    if (working(m)) {
        hint(m, std::format("{} from the next request", thinking_name(c.level)), now);
    } else {
        hint(m, std::format("Thinking {} \xE2\x86\x92 {}{}", thinking_name(was.level), thinking_name(c.level), once_said(m)), now);
    }
    return changed(m, c);
}

namespace {

// The menu's groups for its search and the providers' states: a provider with no key in the keys
// file has none whatever the agent said last.
void regroup(Model& m) {
    std::vector<events::ProviderState> states = m.providers;
    if (!m.has_key) {
        states.push_back({std::string{ai::catalog::default_provider().id}, events::Trouble::NoKey});
    }
    m.menu.groups = picker::groups(m.menu.query.text, states);
}

} // namespace

void open_menu(Model& m) {
    if (m.permission || m.key_mode) { // a question or the key's field has the keys: no menu over it (review I1)
        return;
    }
    m.menu.open = true;
    m.menu.query = {};
    m.menu.by_keys = false;
    regroup(m);
    m.menu.active = 0; // the model chosen lit, if it is there
    const std::string_view chosen = effective(m).model;
    for (std::size_t i = 0; i < menu_rows(m); ++i) {
        if (menu_row(m, i)->model->id == chosen) {
            m.menu.active = i;
        }
    }
}

void close_menu(Model& m) {
    m.menu.open = false; // its groups stay: it fades out with them (made again when it opens)
    m.menu.query = {};
    m.preview.reset();
}

auto menu_rows(const Model& m) -> std::size_t {
    std::size_t n = 0;
    for (const auto& g : m.menu.groups) {
        n += g.rows.size();
    }
    return n;
}

auto menu_row(const Model& m, std::size_t i) -> const picker::Row* {
    for (const auto& g : m.menu.groups) {
        if (i < g.rows.size()) {
            return &g.rows[i];
        }
        i -= g.rows.size();
    }
    return nullptr;
}

auto answer_cost(const Model& m, const ai::catalog::Model& model) -> double {
    double sum = 0;
    int n = 0;
    for (auto it = m.answer_costs.rbegin(); it != m.answer_costs.rend() && n < 20; ++it) {
        if (it->model == model.id) {
            sum += it->usd;
            ++n;
        }
    }
    if (n > 0) {
        return sum / n;
    }
    return (3000 * model.prices.cache_miss + 600 * model.prices.output) / 1e6;
}

auto provider_trouble(const Model& m, std::string_view provider) -> events::Trouble {
    if (!m.has_key && provider == ai::catalog::default_provider().id) {
        return events::Trouble::NoKey;
    }
    const auto it = std::ranges::find(m.providers, provider, &events::ProviderState::provider);
    return it == m.providers.end() ? events::Trouble::None : it->trouble;
}

void hover_row(Model& m, std::size_t i) {
    if (i < menu_rows(m)) {
        m.menu.active = i;
        m.menu.by_keys = false;
    }
}

auto pick(Model& m, std::size_t i, bool once) -> Reaction {
    const picker::Row* row = menu_row(m, i);
    if (row == nullptr) {
        return {};
    }
    if (row->dimmed && !row->action.empty()) { // Add key, Update key: the key's field (the settings' in 4d)
        close_menu(m);
        ask_key(m);
        return {};
    }
    // Offline rows are dimmed but chosen as any (review M6): the network may be back by the next
    // request. A lasting choice keeps the setting's level, a one-off the one-off's (review M7).
    ModelChoice c = once ? effective(m) : m.choice;
    c.model = row->model->id;
    close_menu(m);
    return set_choice(m, c, once, m.now);
}

auto view(const Model& m) -> View {
    if (!m.open) {
        return View::Rest; // the handle, whatever goes on behind it (presence_of says what)
    }
    if (m.key_mode) {
        return View::Input; // the bar's field, its text dots
    }
    if (m.permission) {
        return View::Permission;
    }
    if (m.log && m.activity == Activity::Idle) {
        return View::Log;
    }
    if (m.expanded && has_detail(m)) {
        return View::Terminal;
    }
    if (m.report && m.activity != Activity::Thinking && m.activity != Activity::Done) {
        return View::Report;
    }
    if (m.quick && !m.quick_more && m.activity != Activity::Thinking) {
        return View::Quick;
    }
    if (!m.steps.empty() && m.activity != Activity::Done) {
        return View::Steps; // the work as cards under the bar, until the result bar
    }
    switch (m.activity) {
    case Activity::Thinking: return View::Thinking;
    case Activity::Answering: return View::Answering;
    case Activity::Done: return View::Done;
    case Activity::Idle: break;
    }
    return View::Input;
}

auto view_name(View v) -> std::string_view {
    switch (v) {
    case View::Rest: return "rest";
    case View::Input: return "input";
    case View::Thinking: return "thinking";
    case View::Answering: return "answering";
    case View::Done: return "done";
    case View::Permission: return "permission";
    case View::Quick: return "quick";
    case View::Steps: return "steps";
    case View::Log: return "log";
    case View::Report: return "report";
    case View::Terminal: return "terminal";
    }
    return "?";
}

auto wants_dim(const Model& m) -> bool {
    return m.open && m.permission.has_value();
}

void remember(Model& m, std::string_view question) {
    std::string q = trimmed(question);
    if (!q.empty() && (m.recent.empty() || m.recent.front() != q)) {
        m.recent.insert(m.recent.begin(), std::move(q)); // older than anything asked since
    }
}

void resume_tasks(Model& m, std::uint64_t used) {
    m.last_task = std::max(m.last_task, used);
}

void open(Model& m) {
    m.open = true;
    if (!m.has_key && !m.key_mode) { // no agent without a key: the key's field first
        ask_key(m);
    }
    // The handle's task to the front (phase 10, screens 12–16): the work as it goes, the question,
    // the answer, the failure — the sheet opens on it. Watchers stay in the background.
    if (m.task != 0 && (working(m) || m.permission)) {
        return; // a task at work in front stays there
    }
    // What the handle says (a question, an answer, a failure), or the work the closing hid; a task
    // the user sent behind by asking another stays there.
    const auto t = behind_of(m);
    const bool says = t.state == BehindState::Decision || t.state == BehindState::Answer || t.state == BehindState::Failed;
    const auto task = says ? t.task : m.closed_task;
    m.closed_task = 0;
    if (task != 0 && bring_front(m, task)) {
        std::erase_if(m.drops, [&](const Drop& d) { return d.task == task && d.kind != DropKind::Alert; });
    }
}

void close(Model& m) {
    m.open = false;
    close_menu(m); // it goes with the sheet, its preview forgotten
    // A task at work (or asking) goes on behind the handle; the sheet opens on it again.
    if (m.task != 0 && (working(m) || m.permission)) {
        behind(m);
        m.closed_task = m.task;
        const auto keep = m.last_task;
        static_cast<TaskState&>(m) = TaskState{};
        m.last_task = keep;
    }
}

void toggle(Model& m) {
    if (m.open) {
        close(m);
    } else {
        open(m);
    }
}

void type(Model& m, std::string_view text) {
    if (m.key_mode) {
        insert(m.key, text);
        m.key_error.clear(); // a new try: the old message goes
        return;
    }
    if (m.menu.open) { // the model menu's search: its first match lit
        insert(m.menu.query, text);
        regroup(m);
        m.menu.active = 0;
        m.menu.by_keys = false;
        return;
    }
    if (m.filter_focus) {
        insert(m.filter, text); // the panel's filter has the focus
        return;
    }
    if (m.private_focus) {
        insert(m.private_add, text); // Private windows' own field (phase 13a)
        return;
    }
    if (m.permission) {
        insert(m.input, text, true); // a new request (it replaces the asking task), or the advanced word
        return;
    }
    if (m.activity == Activity::Done) {
        m.activity = Activity::Idle; // a new question from the result bar
        m.input = {};
    }
    insert(m.input, text, true);
}

auto char_count(std::string_view text) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

namespace {

auto editor_can(const Model& m) -> bool {
    return m.open && !m.key_mode && !m.filter_focus && !m.private_focus && !working(m) && view(m) != View::Log;
}

} // namespace

void toggle_editor(Model& m) {
    m.editor = !m.editor && editor_can(m);
}

auto editor_shown(const Model& m) -> bool {
    return m.editor && editor_can(m);
}

auto counter_text(std::size_t chars, CounterText& buf, bool always) -> std::string_view {
    if (chars < kCounterFrom && !always) {
        return {};
    }
    // The digits from the right, a no-break space (U+00A0) after each three.
    CounterText rev{};
    std::size_t n = 0;
    for (int group = 0; chars > 0 || n == 0; ++group) {
        if (group > 0 && group % 3 == 0) {
            rev[n++] = '\xA0';
            rev[n++] = '\xC2';
        }
        rev[n++] = static_cast<char>('0' + chars % 10);
        chars /= 10;
    }
    std::size_t k = 0;
    while (n > 0) {
        buf[k++] = rev[--n];
    }
    for (const char c : std::string_view{" chars"}) {
        buf[k++] = c;
    }
    return {buf.data(), k};
}

void dismiss(Model& m) {
    if (m.activity != Activity::Done) {
        return;
    }
    m.activity = Activity::Idle;
    m.input = {};
    m.done_title.clear();
    m.done_detail.clear();
    m.done_actions.clear();
    m.failed = false;
}

void ask_key(Model& m) {
    // Not m.open: opening is the shell's (window styles, focus) — a model already open made its
    // open_sheet() return at once, and the field took neither clicks nor keys.
    m.key_mode = true;
    m.key = {};
    m.key_entered.reset();
    close_menu(m);
}

void after_install(Model& m, bool has_key) {
    if (!has_key) {
        ask_key(m);
    }
}

auto masked(const Edit& e) -> Edit {
    Edit out;
    mask_into(e, out);
    return out;
}

void mask_into(const Edit& e, Edit& out) {
    constexpr std::string_view dot = "\xE2\x80\xA2";
    out.text.clear();
    std::size_t chars = 0, before_caret = 0;
    for (std::size_t i = 0; i < e.text.size(); ++i) {
        if ((static_cast<unsigned char>(e.text[i]) & 0xC0) != 0x80) { // a character starts here
            ++chars;
            before_caret += i < e.caret ? 1 : 0;
        }
    }
    for (std::size_t i = 0; i < chars; ++i) {
        out.text += dot;
    }
    out.caret = before_caret * dot.size();
}

void key_rejected(Model& m, std::string_view why) {
    m.key_mode = true;
    m.key_checking = false;
    m.key_error = std::string{why};
}

void key_saved(Model& m, bool ok, std::string_view error) {
    m.has_key = m.has_key || ok;
    if (ok) { // the menu's "key rejected" goes with the new key (its next answer says again)
        std::erase_if(m.providers, [](const events::ProviderState& s) {
            return s.provider == ai::catalog::default_provider().id &&
                   (s.trouble == events::Trouble::KeyRejected || s.trouble == events::Trouble::NoKey);
        });
    }
    m.key_checking = false;
    m.key_error.clear();
    m.key_mode = false;
    m.key = {};
    m.input = {};
    m.activity = Activity::Done;
    m.streaming = false;
    m.failed = !ok;
    m.done_title = ok ? "Key saved" : "The key could not be saved";
    m.done_detail = ok ? std::string{"encrypted for this Windows user"} : std::string{error};
    m.done_actions.clear();
}

auto can_send(const Model& m) -> bool {
    // Asked every frame (the send disc): looked at in place, never copied.
    const auto blank = [](std::string_view s) { return std::ranges::all_of(s, [](char c) { return is_space(c); }); };
    if (m.key_mode) {
        return !blank(m.key.text);
    }
    return !blank(m.input.text) || std::ranges::any_of(m.attachments, [](const Attachment& a) { return a.refused.empty(); });
}

auto submit(Model& m) -> std::optional<events::Command> {
    if (m.query && m.answering_other) { // the question's own answer, from Send (phase 12 § 4)
        return refine(m);
    }
    if (m.key_mode) {
        // The key to the shell for DeepSeek to check, and out of the sheet's memory; the field waits
        // for the verdict (key_saved, key_rejected). Empty, or a check under way: nothing. Not a
        // DeepSeek key at all ("jdasfd;lajsdf"): said at once, nothing sent anywhere.
        std::string key = trimmed(m.key.text);
        if (key.empty() || m.key_checking) {
            return std::nullopt;
        }
        if (!keycheck::looks_right(key)) {
            m.key_error = std::string{keycheck::kWrongForm};
            m.key = {}; // the row shows why in the empty field; a paste tries again
            return std::nullopt;
        }
        m.key_entered = std::move(key);
        m.key = {};
        m.key_checking = true;
        m.key_error.clear();
        return std::nullopt;
    }
    std::string text = trimmed(m.input.text);
    const bool attached = std::ranges::any_of(m.attachments, [](const Attachment& a) { return a.refused.empty(); });
    if ((text.empty() && !attached) || (m.permission && m.permission->advanced && text == kAllowWord)) {
        return std::nullopt; // the advanced word allows only by its button
    }
    // What goes with it: each file named (so the model can read, convert or look at it), each
    // picture in the request itself.
    std::string request = text;
    std::vector<tools::ImageOut> images;
    std::string shown_as; // the question's words when there are none: what was attached
    std::string pasted;   // or the text pasted as a chip (phase 10)
    for (auto& a : m.attachments) {
        if (!a.refused.empty()) {
            continue;
        }
        if (!a.text.empty()) { // a long paste: the whole text, a block of its own after the question
            request += request.empty() ? "" : request.ends_with('\n') ? "\n" : "\n\n";
            request += a.text;
            if (pasted.empty()) {
                pasted = a.text;
            }
            continue;
        }
        if (!a.path.empty()) {
            request += (request.empty() ? "" : request.ends_with('\n') ? "" : "\n\n");
            request += "Attached: " + a.path + "\n";
        }
        if (a.image) {
            images.push_back(std::move(*a.image));
        }
        shown_as += (shown_as.empty() ? "" : ", ") + a.label;
    }
    while (request.ends_with('\n')) {
        request.pop_back();
    }
    m.attachments.clear();
    m.attach_note.clear();
    if (text.empty() && !pasted.empty()) {
        // Only a pasted text: its first line is the question shown and kept (the model gets it whole).
        std::string_view first = std::string_view{pasted}.substr(0, pasted.find('\n'));
        bool cut = first.size() < pasted.size();
        if (first.size() > kPastedQuestion) {
            std::size_t n = kPastedQuestion;
            while (n > 0 && (static_cast<unsigned char>(first[n]) & 0xC0) == 0x80) {
                --n; // on a character's start
            }
            first = first.substr(0, n);
            cut = true;
        }
        text = std::string{first} + (cut ? "\xE2\x80\xA6" : "");
    } else if (text.empty()) {
        text = shown_as;
    }
    m.pasted_field.reset();
    // Over a question: the new task replaces the asking one, which takes it as a no (the Start
    // names it). A task that works goes to the background and goes on.
    const std::uint64_t replaced = m.permission ? m.task : 0;
    if (!m.permission && working(m)) {
        m.background.push_back(static_cast<const TaskState&>(m));
    } else if (!m.question.empty()) {
        archive(m); // the turn before: folded, newest first, its data kept
    }
    m.data.clear();
    m.activity_log.clear();
    m.time = m.clock;
    m.started_at = m.now;
    m.ended_at = -1;
    m.permission.reset();
    m.answered.reset();
    m.task = next_task(m);
    m.question = text;
    remember_last(m, text);
    m.input = {};
    m.editor = false; // sent from the editor: the row folds back with the field
    m.activity = Activity::Thinking;
    m.running = true;
    m.thinking_since = m.now; // the model thinks from the request on
    m.answer_pressed = false;
    m.step.clear();
    m.answer.clear();
    m.streaming = false;
    m.actions.clear();
    m.done_title.clear();
    m.done_detail.clear();
    m.done_actions.clear();
    m.failed = false;
    clear_work(m);
    for (const auto& image : images) { // the turn shows what the model was shown
        m.pictures.push_back(events::picture_of(image));
        m.pictures.back().mine = true; // the user's own: in their message (phase 10)
    }
    const bool fresh = std::exchange(m.fresh_chat, false);
    events::Start start{.task = m.task, .text = std::move(request), .replaces = replaced, .fresh = fresh, .images = std::move(images)};
    const auto once = std::exchange(m.next_only, std::nullopt);
    if (once) { // "only for the next request": this one
        start.model = std::string{once->model};
        start.thinking = std::string{thinking_id(once->level)};
    }
    // The model it goes to: the chosen one, or — with pictures and a model that does not see — one that does.
    const auto& asked = model_of(once ? *once : m.choice);
    const auto* sees = ai::catalog::beside(asked).vision;
    m.asked_model = !start.images.empty() && asked.vision != ai::catalog::Tri::Yes && sees != nullptr ? sees->id : asked.id;
    return start;
}

namespace {

auto room_for_one(Model& m) -> bool {
    if (m.attachments.size() < kMaxAttachments) {
        return true;
    }
    m.attach_note = "At most 6 attachments";
    return false;
}

auto leaf_of(std::string_view path) -> std::string {
    const auto slash = path.find_last_of("\\/");
    return std::string{slash == std::string_view::npos ? path : path.substr(slash + 1)};
}

} // namespace

void paste_picture(Model& m, tools::ImageOut image, double now) {
    if (room_for_one(m)) {
        auto thumb = events::picture_of(image);
        thumb.id = ++m.pictures_taken;
        auto meta = details_line(image.mime == "image/png" ? "png" : "jpg", image.bytes.size(), image.source_width, image.source_height, 0, 0);
        m.attachments.push_back({.label = image.label, .path = {}, .image = std::move(image), .refused = {}, .thumb = std::move(thumb),
                                 .meta = std::move(meta), .pasted_at = now});
    }
}

auto conversation_view(const Model& m, View v) -> bool {
    // Rules and memory's list (phase 12 § 6) is in the panel even with no conversation yet.
    const bool memory = std::ranges::any_of(m.sets, [&](const events::DataSet& s) {
        return s.id == m.active_set && s.kind == events::SetKind::Memory;
    });
    if (m.key_mode || (m.question.empty() && m.session.empty() && !memory)) {
        return false;
    }
    switch (v) {
    case View::Input:
    case View::Thinking:
    case View::Answering:
    case View::Steps: return true;
    case View::Done: return !m.question.empty(); // a done bar of its own (a key saved) stays one
    case View::Permission: return !m.question.empty(); // its card in the turn, its objects in the panel
    case View::Terminal: return !m.question.empty(); // the plan in the turn, the terminal in the panel
    case View::Report: return !m.question.empty();     // its sentence and advice here, its numbers in the panel
    default: return false;
    }
}

auto attachments_shown(const Model& m) -> bool {
    const View v = view(m);
    return m.open && !m.key_mode && (v == View::Input || conversation_view(m, v));
}

auto paste(Model& m, std::string_view text, double now) -> bool {
    if (m.filter_focus || m.private_focus || !attachments_shown(m)) { // no row of chips to show it in: typed
        type(m, text);
        return false;
    }
    if (m.key_mode) {
        type(m, text);
        return false;
    }
    // Its line breaks as the field keeps them; its tabs kept - a range from Excel keeps its columns
    // for the model; put in the field they become spaces as ever.
    Edit clean;
    clean.text.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                continue; // CRLF: one, from the LF
            }
            clean.text.push_back('\n');
        } else if (c == '\n' || c == '\t' || static_cast<unsigned char>(c) >= 0x20 || c < 0) {
            clean.text.push_back(c);
        }
    }
    const auto breaks = static_cast<std::size_t>(std::ranges::count(clean.text, '\n'));
    const std::size_t lines = clean.text.empty() ? 0 : breaks + (clean.text.ends_with('\n') ? 0 : 1);
    const std::size_t chars = char_count(clean.text);
    if ((lines <= kPasteLines && chars <= kPasteChars) || !room_for_one(m)) {
        type(m, text);
        return false;
    }
    type(m, {}); // a new question from the result bar, as typing starts one
    CounterText count{};
    auto meta = std::format("{} {} \xC2\xB7 {}", lines, lines == 1 ? "line" : "lines", counter_text(chars, count, true));
    m.attachments.push_back({.label = "Pasted text", .path = {}, .image = {}, .refused = {}, .thumb = {}, .meta = std::move(meta),
                             .pasted_at = now, .text = std::move(clean.text)});
    m.pasted_field = m.input.text;
    return true;
}

auto paste_notice(const Model& m) -> bool {
    return m.pasted_field && *m.pasted_field == m.input.text && !m.key_mode && !editor_shown(m) &&
           std::ranges::any_of(m.attachments, [](const Attachment& a) { return !a.text.empty(); });
}

auto paste_as_text(Model& m) -> bool {
    if (!paste_notice(m)) {
        return false;
    }
    const auto it = std::ranges::find_if(m.attachments.rbegin(), m.attachments.rend(), [](const Attachment& a) { return !a.text.empty(); });
    const std::string text = std::move(it->text);
    m.attachments.erase(std::next(it).base());
    insert(m.input, text, true);
    m.pasted_field.reset();
    return true;
}

auto paste_chip_as_text(Model& m, std::size_t index) -> bool {
    if (index >= m.attachments.size() || m.attachments[index].text.empty()) {
        return false;
    }
    const std::string text = std::move(m.attachments[index].text);
    m.attachments.erase(m.attachments.begin() + static_cast<std::ptrdiff_t>(index));
    insert(m.input, text, true);
    m.pasted_field.reset();
    return true;
}

void open_pasted(Model& m, std::size_t index) {
    if (paste_chip_as_text(m, index)) {
        m.editor = m.open && !m.key_mode;
    }
}

void drop_files(Model& m, std::span<const std::string> paths, const std::function<std::string(const std::string&)>& refusal) {
    for (const auto& p : paths) {
        if (!room_for_one(m)) {
            return;
        }
        auto refused = refusal ? refusal(p) : std::string{};
        auto meta = refused.empty() ? std::string{} : "Won't be sent \xC2\xB7 " + refused;
        m.attachments.push_back({.label = leaf_of(p), .path = p, .image = {}, .refused = std::move(refused), .thumb = {}, .meta = std::move(meta)});
    }
}

void attach_image(Model& m, std::string_view path, tools::ImageOut image) {
    for (auto& a : m.attachments) {
        if (a.path == path && a.refused.empty()) {
            a.thumb = events::picture_of(image);
            a.thumb->id = ++m.pictures_taken;
            a.image = std::move(image);
            return;
        }
    }
}

auto picture_file(std::string_view path) -> bool {
    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos || path.find_first_of("\\/", dot) != std::string_view::npos) {
        return false;
    }
    std::string ext{path.substr(dot)};
    std::ranges::transform(ext, ext.begin(), [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
    static constexpr std::string_view kPictures[] = {".jpg", ".jpeg", ".png", ".gif", ".bmp", ".tif", ".tiff", ".webp", ".heic", ".ico"};
    return std::ranges::find(kPictures, ext) != std::end(kPictures);
}

auto program_file(std::string_view path) -> bool {
    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos || path.find_first_of("\\/", dot) != std::string_view::npos) {
        return false;
    }
    std::string ext{path.substr(dot)};
    std::ranges::transform(ext, ext.begin(), [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
    static constexpr std::string_view kRun[] = {".exe", ".scr", ".com", ".bat", ".cmd", ".msi", ".msp", ".pif", ".lnk", ".url", ".js",
                                                ".jse", ".vbs", ".vbe", ".wsf", ".wsh", ".ps1", ".hta", ".cpl", ".reg", ".jar", ".appref-ms"};
    return std::ranges::find(kRun, ext) != std::end(kRun);
}

auto opens_safely(std::string_view path) -> bool {
    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos || path.find_first_of("\\/", dot) != std::string_view::npos) {
        return false;
    }
    std::string ext{path.substr(dot)};
    std::ranges::transform(ext, ext.begin(), [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
    static constexpr std::string_view kSafe[] = {
        ".pdf", ".txt", ".md", ".rtf", ".log", ".csv", ".tsv", ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".odt", ".ods", ".odp",
        ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".tif", ".tiff", ".webp", ".heic", ".ico", ".mp3", ".wav", ".flac", ".m4a", ".ogg",
        ".aac", ".mp4", ".mkv", ".avi", ".mov", ".wmv", ".webm", ".m4v", ".zip", ".7z", ".rar", ".epub", ".djvu"};
    return std::ranges::find(kSafe, ext) != std::end(kSafe);
}

auto placed(const Model& m, std::string_view path) -> const events::Place* {
    const auto it = std::ranges::find(m.places, path, &events::Place::path);
    return path.empty() || it == m.places.end() ? nullptr : &*it;
}

void set_details(Model& m, std::string_view path, std::string details) {
    for (auto& a : m.attachments) {
        if (a.path == path && a.refused.empty()) {
            a.meta = std::move(details);
        }
    }
}

void remove_attachment(Model& m, std::size_t index) {
    if (index < m.attachments.size()) {
        m.attachments.erase(m.attachments.begin() + static_cast<std::ptrdiff_t>(index));
        m.attach_note.clear();
    }
}

auto refine(Model& m) -> std::optional<events::Command> {
    std::string text = trimmed(m.input.text);
    if (text.empty() || !working(m)) {
        return std::nullopt;
    }
    m.input = {};
    if (m.query && m.answering_other) { // the question's "Other": their own answer (phase 12 § 4)
        return replied(m, {}, std::move(text), true);
    }
    if (m.query) { // a message instead of a reply: the agent answers "not answered" and passes it on
        m.query.reset();
        m.picks.clear();
    }
    return events::Refine{m.task, std::move(text)};
}

auto stop(Model& m) -> std::optional<events::Command> {
    if (!working(m)) {
        return std::nullopt;
    }
    const std::uint64_t task = m.task;
    m.task = next_task(m); // whatever the stopped task still sends is ignored
    m.streaming = false;
    m.running = false;
    settle_steps(m.steps, events::StepState::Skipped);
    if (m.permission && m.permission->advanced) {
        m.input = {}; // the half-typed word, not a question
    }
    m.permission.reset(); // the agent takes the Cancel as a no
    m.query.reset();      // and as no reply
    if (m.answer.empty()) {
        m.activity = Activity::Idle;
        m.input.text = m.question; // back to editing the question
        m.input.caret = m.input.text.size();
        m.input.anchor = Edit::kNoAnchor;
    } else {
        m.activity = Activity::Answering;
    }
    return events::Cancel{task};
}

// Check deeper on a set's rows (the Startup tab, final review I7): "deeper:<id>" of a set that shows it.
auto deeper_row(const std::vector<events::DataSet>& sets, std::string_view action) -> bool {
    if (!action.starts_with("deeper:")) {
        return false;
    }
    std::uint32_t id = 0;
    const auto digits = action.substr(7);
    if (std::from_chars(digits.data(), digits.data() + digits.size(), id).ec != std::errc{}) {
        return false;
    }
    for (const auto& s : sets) {
        if (s.id != id || s.columns.empty() || s.columns.back().kind != ui::work::ColumnKind::Action || s.rows.columns == 0) {
            continue;
        }
        for (std::size_t r = 0; r < s.rows.rows(); ++r) {
            if (s.rows.at(r, s.rows.columns - 1).text == "Check deeper") {
                return true;
            }
        }
    }
    return false;
}

auto press(Model& m, std::string_view action) -> std::optional<events::Command> {
    const auto offered = [&](const std::vector<events::Action>& list) {
        return std::ranges::any_of(list, [&](const events::Action& a) { return a.id == action; });
    };
    if (action == "set_key" && (offered(m.done_actions) || offered(m.actions))) {
        ask_key(m); // the sheet's own: nothing for the agent
        return std::nullopt;
    }
    const bool quick = m.quick && m.quick->action && m.quick->action->id == action;
    // The report's Close (a program it offers to close) and Do it.
    const bool report = m.report && (std::ranges::any_of(m.report->advice, [&](const events::Advice& a) { return a.action == action; }) ||
                                     std::ranges::any_of(m.report->processes, [&](const events::ProcRow& p) {
                                         return p.closable && action == "close:" + std::to_string(p.pid);
                                     }));
    const bool take_over = action == "take_over" && has_detail(m) && working(m); // the user's own terminal
    if (take_over) {
        return events::Act{m.task, std::string{action}}; // the task waits; nothing else changes here
    }
    // A set's row button (phase 13a): Stop ("close:<pid>") or Uninstall… ("uninstall:<n>"),
    // only as a set shows it — its cell's number.
    const auto row_button = [&](std::string_view prefix, std::string_view label) {
        if (!action.starts_with(prefix)) {
            return false;
        }
        double n = -1;
        const auto digits = action.substr(prefix.size());
        if (std::from_chars(digits.data(), digits.data() + digits.size(), n).ec != std::errc{}) {
            return false;
        }
        return std::ranges::any_of(m.sets, [&](const events::DataSet& s) {
            for (std::size_t c = 0; c < s.columns.size() && c < s.rows.columns; ++c) {
                if (s.columns[c].kind != ui::work::ColumnKind::Action) {
                    continue;
                }
                for (std::size_t r = 0; r < s.rows.rows(); ++r) {
                    const auto& cell = s.rows.at(r, c);
                    if (cell.number == n && cell.text.starts_with(label)) {
                        return true;
                    }
                }
            }
            return false;
        });
    };
    const bool row = row_button("close:", "Stop") || row_button("uninstall:", "Uninstall");
    // The threat report's buttons (phase 13a, 05c/05g): a finding's action ("finding:<row>:<action>"),
    // and Continue from step N ("check_from:<N>"), only as a report shows them.
    const auto threat_button = [&] {
        const auto number = [](std::string_view s, std::size_t& n) { return std::from_chars(s.data(), s.data() + s.size(), n).ec == std::errc{}; };
        return std::ranges::any_of(m.sets, [&](const events::DataSet& s) {
            if (!s.threat) {
                return false;
            }
            std::size_t a = 0, b = 0;
            if (action.starts_with("finding:")) {
                const auto rest = action.substr(8);
                const auto colon = rest.find(':');
                return colon != std::string_view::npos && number(rest.substr(0, colon), a) && number(rest.substr(colon + 1), b) &&
                       a < s.threat->rows.size() && b < s.threat->rows[a].actions.size();
            }
            // Check deeper: a finished report with something not read, not deepened yet, by its own id.
            if (action.starts_with("deeper:")) {
                return number(action.substr(7), a) && a == s.id && !s.threat->stopped_at && !s.threat->not_read.empty() &&
                       !s.threat->deeper;
            }
            return action.starts_with("check_from:") && number(action.substr(11), a) && s.threat->stopped_at &&
                   static_cast<std::size_t>(*s.threat->stopped_at) == a;
        });
    };
    const bool threat = threat_button() || deeper_row(m.sets, action);
    if (!offered(m.actions) && !offered(m.done_actions) && !quick && !report && !row && !threat) {
        return std::nullopt;
    }
    if (action == "retry" && m.failed) {
        // The turn is tried again from its start: its words go, the failure with them.
        if (!m.failed_partial.empty() && m.answer.ends_with(m.failed_partial)) {
            m.answer.resize(m.answer.size() - m.failed_partial.size());
        }
        m.failed = false;
        m.failed_partial.clear();
        m.done_title.clear();
        m.done_detail.clear();
    }
    m.activity = Activity::Thinking;
    m.running = true;
    m.step.clear();
    m.actions.clear();
    m.done_actions.clear();
    m.quick.reset(); // its action is the work now
    return events::Act{m.task, std::string{action}};
}

void apply(Model& model, const events::Event& e) {
    if (const auto* s = std::get_if<events::Spent>(&e.event); s != nullptr && !s->model.empty()) { // the menu's tooltip
        if (model.answer_costs.size() >= kAnswerCosts) {
            model.answer_costs.erase(model.answer_costs.begin());
        }
        model.answer_costs.push_back({s->model, s->usd});
    }
    if (const auto* s = std::get_if<events::ProviderState>(&e.event)) { // the menu's rows (stage 4b)
        auto it = std::ranges::find(model.providers, s->provider, &events::ProviderState::provider);
        if (it == model.providers.end()) {
            model.providers.push_back(*s);
        } else {
            it->trouble = s->trouble;
        }
        if (model.menu.open) {
            regroup(model);
            model.menu.active = std::min(model.menu.active, menu_rows(model) == 0 ? 0 : menu_rows(model) - 1);
        }
        return;
    }
    // A watcher's word reaches the sheet whatever task is in front: it lives in the background.
    if (const auto* a = std::get_if<events::Alert>(&e.event)) {
        // The watcher's newest word in its one card: they never pile up.
        std::erase_if(model.drops, [&](const Drop& d) { return d.task == e.task && d.kind == DropKind::Alert; });
        model.drops.push_back({.task = e.task, .kind = DropKind::Alert, .title = a->title, .text = a->detail, .actions = {}});
        ++model.drops_made;
        model.drops.back().made = model.drops_made;
        return;
    }
    if (const auto* w = std::get_if<events::Watching>(&e.event)) {
        auto it = std::ranges::find(model.watchers, e.task, &Watcher::task);
        if (it == model.watchers.end()) {
            model.watchers.push_back({.task = e.task, .title = w->title, .detail = w->detail, .on = w->on});
        } else {
            *it = {.task = e.task, .title = w->title, .detail = w->detail, .on = w->on};
        }
        if (e.task == model.task && model.activity != Activity::Done) {
            model.activity = Activity::Done; // said in the bar; the watcher itself goes on in the background
            model.streaming = false;
            model.running = false;
            model.done_title = w->title;
            model.done_detail = w->detail;
            model.done_actions.clear();
        }
        return;
    }
    TaskState* target = owner(model, e.task);
    if (target == nullptr) {
        return;
    }
    if (const auto* ctx = std::get_if<events::Context>(&e.event)) {
        if (target == &model) { // the conversation in front (phase 12 § 10)
            model.context = ctx->percent;
        }
        return;
    }
    if (const auto* job = std::get_if<events::JobDone>(&e.event)) {
        // A background command ended (phase 12 § 8): in front with the sheet open the conversation
        // says it; otherwise a card that goes as a notification at once.
        if (target == &model && model.open) {
            return;
        }
        const std::string title = job->stopped     ? std::format("`{}` stopped", job->command)
                                  : job->timed_out ? std::format("`{}` ended after 2 hours", job->command)
                                                   : std::format("`{}` finished: exit {}", job->command, job->code);
        model.drops.push_back({.task = e.task, .kind = DropKind::Result, .title = title, .text = {}, .actions = {}});
        model.drops.back().failed = job->code != 0 || job->stopped || job->timed_out;
        model.drops.back().notify = true;
        ++model.drops_made;
        model.drops.back().made = model.drops_made;
        return;
    }
    // A threat check's report: the task's own, for the handle's words when it ends behind (phase 13a).
    if (const auto* d = std::get_if<events::Data>(&e.event); d != nullptr && d->set.threat) {
        target->threat = d->set.threat;
    }
    if (target != &model) {
        // A background task: its question and its result drop from the tab as cards.
        const auto card = [&](DropKind kind, std::string title, std::string text, std::vector<events::Action> actions) {
            std::erase_if(model.drops, [&](const Drop& d) { return d.task == e.task && d.kind != DropKind::Alert; });
            model.drops.push_back({.task = e.task, .kind = kind, .title = std::move(title), .text = std::move(text),
                                   .actions = std::move(actions)});
            ++model.drops_made;
            model.drops.back().made = model.drops_made;
        };
        if (const auto* p = std::get_if<events::Permission>(&e.event)) {
            card(DropKind::Question, "Needs a decision", p->title.empty() ? p->reason : p->title, {});
        } else if (const auto* q = std::get_if<events::Question>(&e.event)) {
            card(DropKind::Question, "Needs a decision", q->question, {}); // the model's question (phase 12 § 4)
        } else if (const auto* f = std::get_if<events::Finished>(&e.event)) {
            card(DropKind::Result, f->title, f->detail, f->actions);
        } else if (const auto* x = std::get_if<events::Failed>(&e.event)) {
            card(DropKind::Result, "It did not work", x->message, {});
            model.drops.back().failed = true;
        } else if (const auto* g = std::get_if<events::Actions>(&e.event); g && go_on(*g)) {
            // Stopped at its budget behind the handle: a decision, not work (yellow, no clock).
            card(DropKind::Question, "Needs a decision", last_paragraph(target->answer), g->actions);
        } else if (const auto* ended = std::get_if<events::Actions>(&e.event); ended && !go_on(*ended) && target->threat) {
            // A threat check ended behind the handle (07a): its own words; red only for "suspicious".
            const auto& v = *target->threat;
            std::string text;
            std::size_t red = 0;
            if (v.stopped_at) {
                text = v.checked + " checked";
            } else if (v.suspicious > 0) {
                text = std::format("{} suspicious", v.suspicious);
                red = text.size();
                if (v.worth > 0) {
                    text += std::format(" \xC2\xB7 {} to look at", v.worth);
                }
            } else {
                text = v.worth > 0 ? std::format("nothing suspicious \xC2\xB7 {} to look at", v.worth) : "nothing suspicious \xC2\xB7 " + v.checked;
            }
            card(DropKind::Result, v.stopped_at ? "Check stopped" : "Check done", std::move(text), {});
            auto& d = model.drops.back();
            d.answer = true;
            d.red = red;
            d.quiet = v.stopped_at.has_value();
            d.more = v.verdict; // the preview's line: the verdict in words
        } else if (const auto* a = std::get_if<events::Actions>(&e.event); a && !go_on(*a)) {
            // Its answer is complete behind the handle: green until read (screen 14).
            const auto first = first_sentence(target->answer);
            card(DropKind::Result, "Answer ready", std::string{first}, {});
            model.drops.back().answer = true;
            if (!first.empty()) { // the next sentence: the preview's second line
                const auto at = static_cast<std::size_t>(first.data() + first.size() - target->answer.data());
                model.drops.back().more = std::string{first_sentence(std::string_view{target->answer}.substr(at))};
            }
        }
    }
    if (const auto* p = std::get_if<events::Permission>(&e.event); p && target == &model) {
        // A new question: its own choices, all items ticked.
        model.ticks.assign(p->items.size(), true);
        model.permanent = false;
        model.folder_tick = false;
        model.all_shown = false;
        model.pick = 0;
        close_menu(model); // the card takes the menu's place, and its keys
    }
    TaskState& m = *target;
    std::visit(
        [&](const auto& ev) {
            using T = std::decay_t<decltype(ev)>;
            if constexpr (std::is_same_v<T, events::Thinking>) {
                m.step = ev.step;
                if (m.activity != Activity::Answering) {
                    m.activity = Activity::Thinking;
                }
                if (!m.thinking_since) { // a new thought (after a step, a question)
                    m.thinking_since = model.now;
                    m.answer_pressed = false;
                }
            } else if constexpr (std::is_same_v<T, events::Delta>) {
                m.answer += ev.text;
                m.streaming = true;
                m.activity = Activity::Answering;
            } else if constexpr (std::is_same_v<T, events::Actions>) {
                m.streaming = false;
                m.actions = ev.actions;
                m.activity = Activity::Answering;
                m.running = go_on(ev); // "Go on?" waits for the user: the task is not over
            } else if constexpr (std::is_same_v<T, events::Permission>) {
                m.answered.reset(); // the next question takes the last one's place
                m.permission = ev;
                if (m.permission->picture) { // a new screen, even in the last one's buffer
                    m.permission->picture->id = ++model.pictures_taken;
                }
                if (m.permission->capture) { // its panel's larger picture: a texture of its own (phase 10)
                    m.permission->capture->preview.id = ++model.pictures_taken;
                }
                m.streaming = false;
                if (&m == &model) {
                    model.filter_focus = false; // the panel shows its objects: the typing goes to the field
                    model.private_focus = false;
                    model.retaking = false;     // a screen taken again has come
                }
                if (m.activity != Activity::Answering) {
                    m.activity = Activity::Thinking;
                }
            } else if constexpr (std::is_same_v<T, events::Finished>) {
                m.permission.reset();
                m.streaming = false;
                m.running = false;
                m.done_title = ev.title;
                m.done_detail = ev.detail;
                m.done_actions = ev.actions;
                m.failed = false;
                m.activity = Activity::Done;
                settle_steps(m.steps, events::StepState::Done);
            } else if constexpr (std::is_same_v<T, events::Failed>) {
                m.permission.reset();
                m.streaming = false;
                m.running = false;
                m.done_title = ev.message;
                m.done_detail.clear();
                m.done_actions = ev.actions;
                m.failed = true;
                m.failed_partial = ev.partial;
                m.activity = Activity::Done;
                settle_steps(m.steps, events::StepState::Failed);
            } else if constexpr (std::is_same_v<T, events::Step>) {
                const auto it = std::ranges::find(m.steps, ev.id, &events::Step::id);
                if (it != m.steps.end()) {
                    const auto since = it->since; // when it began, and what runs it, stay as first said
                    auto tool = std::move(it->tool);
                    *it = ev;
                    it->since = it->since < 0 ? since : it->since;
                    if (it->tool.empty()) {
                        it->tool = std::move(tool);
                    }
                } else {
                    m.steps.push_back(ev);
                    if (m.steps.back().since < 0) {
                        m.steps.back().since = model.now;
                    }
                }
            } else if constexpr (std::is_same_v<T, events::Summary>) {
                m.summary = ev;
            } else if constexpr (std::is_same_v<T, events::Progress>) {
                m.progress = std::max(m.progress, ev.fraction); // MS 05: the rail only grows (none is under 0)
            } else if constexpr (std::is_same_v<T, events::Quick>) {
                m.quick = ev;
                m.quick_more = false;
                m.streaming = false;
                if (m.activity == Activity::Thinking) {
                    m.activity = Activity::Answering; // answered: no stop button, the bar is the answer
                }
            } else if constexpr (std::is_same_v<T, events::Plan>) {
                m.plan = ev;
            } else if constexpr (std::is_same_v<T, events::Checklist>) {
                m.checklist = ev; // whole each time
            } else if constexpr (std::is_same_v<T, events::Remembered>) {
                m.remembered.push_back(ev);
            } else if constexpr (std::is_same_v<T, events::Looked>) {
                m.looked.push_back(ev);
            } else if constexpr (std::is_same_v<T, events::Question>) {
                m.query = ev; // the task waits for the reply, like a permission
                m.streaming = false;
                if (m.activity != Activity::Answering) {
                    m.activity = Activity::Thinking;
                }
                if (&m == &model) {
                    model.picks.assign(ev.options.size(), false);
                    model.answering_other = false;
                }
            } else if constexpr (std::is_same_v<T, events::Output>) {
                auto tab = std::ranges::find(m.tabs, ev.tab, &TaskState::Tab::id);
                if (tab == m.tabs.end()) {
                    m.tabs.push_back({.id = ev.tab, .label = ev.tab_label, .buffer = {}});
                    tab = m.tabs.end() - 1;
                }
                tab->buffer.append(ev.text, ev.step);
            } else if constexpr (std::is_same_v<T, events::Spent>) {
                m.spent = ev;
            } else if constexpr (std::is_same_v<T, events::Report>) {
                m.report = ev;
                // Its words are the model's (responsive text): no English line of the program's over them.
                m.quick_more = false;
                m.streaming = false;
                if (m.activity == Activity::Thinking) {
                    m.activity = Activity::Answering; // answered: no stop button, the bar is the answer
                }
            } else if constexpr (std::is_same_v<T, events::Data>) {
                take_data(model, m, ev);
            } else if constexpr (std::is_same_v<T, events::Activity>) {
                m.activity_log.push_back(ev.entry);
            } else if constexpr (std::is_same_v<T, events::Suggestions>) {
                m.suggestions = ev.prompts;
            } else if constexpr (std::is_same_v<T, events::Refusal>) {
                m.refusals.push_back(ev);
            } else if constexpr (std::is_same_v<T, events::Created>) {
                m.created.push_back(ev);
            } else if constexpr (std::is_same_v<T, events::Places>) {
                m.places.clear();
                for (const auto& p : ev.places) { // a file this turn made has its own card (paths in any case)
                    const auto same = [&](const events::Created& c) {
                        return std::ranges::equal(c.path, p.path, [](char a, char b) {
                            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                        });
                    };
                    if (std::ranges::none_of(m.created, same)) {
                        m.places.push_back(p);
                    }
                }
            } else if constexpr (std::is_same_v<T, events::Picture>) {
                m.pictures.push_back(ev);
                m.pictures.back().id = ++model.pictures_taken;
            } else if constexpr (std::is_same_v<T, events::Word> || std::is_same_v<T, events::Phrase> ||
                                 std::is_same_v<T, events::TextTranslation>) {
                if constexpr (std::is_same_v<T, events::Word>) {
                    m.word = ev;
                } else if constexpr (std::is_same_v<T, events::Phrase>) {
                    m.phrase = ev;
                } else {
                    m.text_translation = ev;
                }
                if (m.activity == Activity::Thinking) {
                    m.activity = Activity::Answering; // answered: the card is the answer
                }
            } else if constexpr (std::is_same_v<T, events::Conversation>) {
                if (!ev.continues && &m == &model) {
                    end_session(model);
                }
            }
            // Words, a step, a question, an end: the thought it was is over (phase 12 § 9).
            if constexpr (std::is_same_v<T, events::Delta> || std::is_same_v<T, events::Step> || std::is_same_v<T, events::Permission> ||
                          std::is_same_v<T, events::Actions> || std::is_same_v<T, events::Finished> ||
                          std::is_same_v<T, events::Failed> || std::is_same_v<T, events::Quick> || std::is_same_v<T, events::Report> ||
                          std::is_same_v<T, events::Question>) {
                m.thinking_since.reset();
                m.answer_pressed = false;
            }
            if constexpr (std::is_same_v<T, events::Finished> || std::is_same_v<T, events::Failed>) {
                m.query.reset(); // the task is over: its question with it
            }
            // The turn's answer is complete: how long it took.
            if constexpr (std::is_same_v<T, events::Actions> || std::is_same_v<T, events::Finished> ||
                          std::is_same_v<T, events::Failed> || std::is_same_v<T, events::Quick> ||
                          std::is_same_v<T, events::Report>) {
                if (m.ended_at < 0 && !m.running) {
                    m.ended_at = model.now;
                }
            }
        },
        e.event);
}

void tick(Model& m, std::size_t item) {
    if (m.permission && item < m.ticks.size()) {
        m.ticks[item] = !m.ticks[item];
    }
}

void tick_all(Model& m, bool on) {
    if (m.permission) {
        std::ranges::fill(m.ticks, on);
    }
}

void choose_permanent(Model& m, bool on) {
    if (m.permission && m.permission->offers_permanent) {
        m.permanent = on;
    }
}

void pick_choice(Model& m, std::size_t i) {
    if (m.permission && i < m.permission->picks.size()) {
        m.pick = i;
    }
}

void tick_folder(Model& m) {
    if (m.permission && (m.permission->folder || !m.permission->task_scope.empty())) {
        m.folder_tick = !m.folder_tick;
    }
}

auto spent_price(const events::Spent& s) -> std::string {
    if (s.usd < 0.0001) {
        return "<$0.0001";
    }
    return s.usd < 1.0 ? std::format("${:.4f}", s.usd) : std::format("${:.2f}", s.usd);
}

auto spent_cached(const events::Spent& s) -> std::string {
    if (s.prompt <= 0) {
        return {};
    }
    return std::format("{:.0f}% cached", 100.0 * static_cast<double>(s.cache_hit) / static_cast<double>(s.prompt));
}

auto spent_text(const events::Spent& s) -> std::string {
    std::string out = spent_price(s);
    if (const auto cached = spent_cached(s); !cached.empty()) {
        out += " \xC2\xB7 " + cached;
    }
    return out;
}

auto retake(Model& m, events::Answer::Take take, int monitor, std::string window) -> std::optional<events::Command> {
    if (!m.permission || !m.permission->capture || m.retaking) {
        return std::nullopt;
    }
    m.retaking = true;
    events::Answer a{.task = m.task, .id = m.permission->id, .choice = events::Choice::Retake, .keep = {}, .permanent = false};
    a.take = take;
    a.monitor = monitor;
    a.window = std::move(window);
    return a;
}

auto choose(Model& m, std::size_t option) -> std::optional<events::Command> {
    if (!m.query || option >= m.query->options.size()) {
        return std::nullopt;
    }
    if (m.query->multi) {
        m.picks.resize(m.query->options.size(), false);
        m.picks[option] = !m.picks[option];
        return std::nullopt;
    }
    return replied(m, {m.query->options[option].label}, {}, true);
}

auto send_choices(Model& m) -> std::optional<events::Command> {
    if (!m.query || !m.query->multi) {
        return std::nullopt;
    }
    std::vector<std::string> chosen;
    for (std::size_t i = 0; i < m.picks.size() && i < m.query->options.size(); ++i) {
        if (m.picks[i]) {
            chosen.push_back(m.query->options[i].label);
        }
    }
    if (chosen.empty()) {
        return std::nullopt; // nothing ticked: Skip says that
    }
    return replied(m, std::move(chosen), {}, true);
}

auto skip_question(Model& m) -> std::optional<events::Command> {
    if (!m.query) {
        return std::nullopt;
    }
    return replied(m, {}, {}, false);
}

auto forget_remembered(Model& m, std::uint64_t id, bool private_window) -> std::optional<events::Command> {
    // A memory entry and a private windows rule may share a number: the kind tells them apart.
    if (std::erase_if(m.remembered, [&](const events::Remembered& r) { return r.id == id && r.private_window == private_window; }) == 0) {
        return std::nullopt;
    }
    return events::Forget{.id = id, .all = false, .private_window = private_window};
}

void show_memory(Model& m, events::DataSet set) {
    std::erase_if(m.sets, [](const events::DataSet& s) { return s.kind == events::SetKind::Memory; });
    const auto id = set.id;
    m.sets.push_back(std::move(set));
    activate(m, id);
}

auto forget_row(Model& m, std::uint32_t set, std::size_t row) -> std::optional<events::Command> {
    const auto it = std::ranges::find(m.sets, set, &events::DataSet::id);
    if (it == m.sets.end() || it->kind != events::SetKind::Memory || row >= it->rows.rows() || it->rows.columns < 3) {
        return std::nullopt;
    }
    const double id = it->rows.at(row, 2).number;
    if (id <= 0) {
        return std::nullopt; // the rules, or everything: the shell's; a built-in sign: nothing
    }
    const auto first = it->rows.cells.begin() + static_cast<std::ptrdiff_t>(row * it->rows.columns);
    it->rows.cells.erase(first, first + static_cast<std::ptrdiff_t>(it->rows.columns));
    if (row < it->rows.group.size()) {
        it->rows.group.erase(it->rows.group.begin() + static_cast<std::ptrdiff_t>(row));
    }
    if (id >= events::kPrivateRow) { // a private windows rule (phase 13a)
        return events::Forget{.id = static_cast<std::uint64_t>(id - events::kPrivateRow), .all = false, .private_window = true};
    }
    return events::Forget{.id = static_cast<std::uint64_t>(id), .all = false};
}

void answer_other(Model& m) {
    if (m.query) {
        m.answering_other = true;
        m.filter_focus = false; // the typing goes to the field
        m.private_focus = false;
    }
}

auto answer_now(Model& m) -> std::optional<events::Command> {
    if (!m.running || m.activity != Activity::Thinking || !m.thinking_since || m.answer_pressed || m.permission) {
        return std::nullopt;
    }
    m.answer_pressed = true;
    return events::Act{m.task, "answer_now"};
}

auto answer(Model& m, events::Choice choice) -> std::optional<events::Command> {
    if (!m.permission) {
        return std::nullopt;
    }
    const auto& p = *m.permission;
    if (choice == events::Choice::Allow && m.folder_tick && p.folder) {
        choice = events::Choice::AllowFolder; // "Don't ask again for this folder" was ticked
    } else if (choice == events::Choice::Allow && m.folder_tick && !p.task_scope.empty()) {
        choice = events::Choice::AllowTask; // "Allow “npm test” for this task" was ticked
    }
    if ((choice == events::Choice::AllowFolder && !p.folder) || (choice == events::Choice::AllowTask && p.task_scope.empty())) {
        return std::nullopt;
    }
    const bool permanent = choice != events::Choice::Deny && m.permanent && p.offers_permanent;
    if (choice != events::Choice::Deny && (p.advanced || permanent) && trimmed(m.input.text) != kAllowWord) {
        return std::nullopt; // the word first
    }
    events::Answer a{.task = m.task, .id = p.id, .choice = choice, .keep = {}, .permanent = permanent};
    if (choice != events::Choice::Deny && !p.picks.empty()) {
        a.keep = {std::min(m.pick, p.picks.size() - 1)}; // the version picked (06b): always named, the first too
    } else if (choice != events::Choice::Deny && !m.ticks.empty()) {
        for (std::size_t i = 0; i < m.ticks.size(); ++i) {
            if (m.ticks[i]) {
                a.keep.push_back(i);
            }
        }
        if (a.keep.empty()) {
            return std::nullopt; // nothing ticked: nothing to allow (Review Focus 2)
        }
        if (a.keep.size() == m.ticks.size()) {
            a.keep.clear(); // all of them: the call as asked
        }
    }
    if (p.advanced || permanent) {
        m.input = {};
    }
    // Answered, not gone (MS 11): its card shrinks into its summary row, its panel stays meanwhile.
    m.answered = Answered{.asked = std::move(*m.permission), .choice = choice, .ticks = m.ticks};
    m.permission.reset();
    if (m.activity != Activity::Answering) {
        m.activity = Activity::Thinking;
    }
    return a;
}

auto show_report(Model& m, std::string_view title, std::string text, std::optional<events::DataSet> set) -> bool {
    if (working(m) || m.permission) {
        return false;
    }
    if (!m.question.empty()) {
        archive(m); // the turn in front stays in the session
    }
    m.data.clear();
    m.activity_log.clear();
    m.time = m.clock;
    m.started_at = m.now;
    m.ended_at = m.now;
    m.task = next_task(m); // what an older task still sends is ignored
    m.activity = Activity::Answering;
    m.question = std::string{title};
    m.answer = std::move(text);
    m.streaming = false;
    m.step.clear();
    m.actions.clear();
    m.input = {};
    m.done_title.clear();
    m.done_detail.clear();
    m.done_actions.clear();
    m.failed = false;
    clear_work(m);
    if (set) {
        set->id = 0x80000000u | static_cast<std::uint32_t>(m.task & 0x7FFFFFFFu); // the shell's own: past the agent's ids
        m.sets.push_back(std::move(*set));
        m.data.push_back(m.sets.back().id);
        show_set(m, m.sets.back().id);
    }
    return true;
}

void show_log(Model& m, events::Log log) {
    if (!m.log) {
        m.input = {}; // the field searches the log from empty
    }
    m.log = std::move(log);
}

void close_log(Model& m) {
    m.log.reset();
    m.input = {};
}

namespace {

// `needle` in `hay`, ASCII letters in any case.
auto contains_folded(std::string_view hay, std::string_view needle) -> bool {
    const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    if (needle.size() > hay.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        std::size_t k = 0;
        while (k < needle.size() && fold(hay[i + k]) == fold(needle[k])) {
            ++k;
        }
        if (k == needle.size()) {
            return true;
        }
    }
    return false;
}

} // namespace

auto log_visible(const Model& m, const events::LogTask& t) -> bool {
    if ((m.log_range == LogRange::Today && t.days_ago > 0) || (m.log_range == LogRange::Week && t.days_ago > 6)) {
        return false;
    }
    // The words without the spaces around them; no allocation (the view asks every frame).
    std::string_view q = m.input.text;
    while (!q.empty() && is_space(q.front())) {
        q.remove_prefix(1);
    }
    while (!q.empty() && is_space(q.back())) {
        q.remove_suffix(1);
    }
    if (q.empty() || contains_folded(t.title, q)) {
        return true;
    }
    return std::ranges::any_of(t.rows, [&](const events::LogRow& r) { return contains_folded(r.name, q); });
}

auto log_selected(const Model& m) -> const events::LogTask* {
    if (!m.log) {
        return nullptr;
    }
    const events::LogTask* first = nullptr;
    for (const auto& t : m.log->tasks) {
        if (!log_visible(m, t)) {
            continue;
        }
        if (t.task == m.log_task) {
            return &t;
        }
        if (first == nullptr) {
            first = &t;
        }
    }
    return first;
}

auto undo(Model& m, std::uint64_t target, std::optional<std::size_t> change) -> std::optional<events::Command> {
    if (working(m) || m.permission) {
        return std::nullopt; // a task is working: one at a time in front
    }
    std::string title = "this task";
    if (m.log) {
        for (const auto& t : m.log->tasks) {
            if (t.task == target) {
                title = "\xE2\x80\x9C" + t.title + "\xE2\x80\x9D";
            }
        }
    }
    m.task = next_task(m);
    m.question = (change ? "Undo a change of " : "Undo ") + title;
    m.activity = Activity::Thinking;
    m.step.clear();
    m.answer.clear();
    m.streaming = false;
    m.actions.clear();
    m.done_title.clear();
    m.done_detail.clear();
    m.done_actions.clear();
    m.failed = false;
    clear_work(m);
    return events::Undo{m.task, target, change};
}

auto undo_last(Model& m) -> std::optional<events::Command> {
    if (!m.log) {
        return std::nullopt;
    }
    for (const auto& t : m.log->tasks) { // newest first
        if (t.undoable) {
            return undo(m, t.task, std::nullopt);
        }
    }
    return std::nullopt;
}

auto opens_sheet(const Model& m, const events::Event& e) -> bool {
    return std::holds_alternative<events::Permission>(e.event) && e.task == m.task && m.permission.has_value() && !m.open;
}

auto has_detail(const TaskState& t) -> bool {
    return t.plan.has_value() || !t.tabs.empty();
}

void expand(Model& m, bool on) {
    m.expanded = on;
    m.hovered_step = -1;
}

auto has_background(const Model& m) -> bool {
    return !m.background.empty() || !m.watchers.empty() || !m.drops.empty();
}

auto overall_progress(const Model& m) -> float {
    float sum = 0;
    int n = 0;
    for (const auto& t : m.background) {
        if (listed(t) && t.progress >= 0) { // a finished task is only its card
            sum += t.progress;
            ++n;
        }
    }
    return n == 0 ? -1.0f : sum / static_cast<float>(n);
}

auto pause_task(Model& m, std::uint64_t task) -> std::optional<events::Command> {
    auto* t = owner(m, task);
    if (t == nullptr || !working(*t)) {
        return std::nullopt;
    }
    t->paused = !t->paused;
    if (t->paused) {
        return events::Pause{task};
    }
    return events::Resume{task};
}

auto cancel_task(Model& m, std::uint64_t task) -> std::optional<events::Command> {
    const auto it = std::ranges::find(m.background, task, &TaskState::task);
    if (it == m.background.end()) {
        return std::nullopt;
    }
    m.background.erase(it); // a question it was waiting on is a no
    std::erase_if(m.drops, [&](const Drop& d) { return d.task == task; });
    return events::Cancel{task};
}

auto toggle_watch(Model& m, std::uint64_t task) -> std::optional<events::Command> {
    const auto it = std::ranges::find(m.watchers, task, &Watcher::task);
    if (it == m.watchers.end()) {
        return std::nullopt;
    }
    return events::Toggle{task, !it->on};
}

auto remove_watch(Model& m, std::uint64_t task) -> std::optional<events::Command> {
    const auto it = std::ranges::find(m.watchers, task, &Watcher::task);
    if (it == m.watchers.end() || it->on) {
        return std::nullopt;
    }
    m.watchers.erase(it);
    std::erase_if(m.drops, [&](const Drop& d) { return d.task == task; });
    return events::Cancel{task};
}

auto behind_of(const Model& m) -> Behind {
    Behind t;
    const auto task_of = [&](std::uint64_t id) -> const TaskState* {
        const auto it = std::ranges::find(m.background, id, &TaskState::task);
        return it == m.background.end() ? nullptr : &*it;
    };
    const auto watchers_on = static_cast<std::size_t>(std::ranges::count_if(m.watchers, &Watcher::on));
    // Newest first: a decision, then a failure, then an answer (or what a watcher saw).
    for (auto d = m.drops.rbegin(); d != m.drops.rend(); ++d) {
        if (d->kind == DropKind::Question) {
            return {.state = BehindState::Decision, .task = d->task, .text = d->text};
        }
    }
    for (auto d = m.drops.rbegin(); d != m.drops.rend(); ++d) {
        if (d->kind == DropKind::Result && d->failed) {
            return {.state = BehindState::Failed, .task = d->task, .text = d->text};
        }
    }
    if (!m.drops.empty()) { // the newest result, or what a watcher saw
        const auto& d = m.drops.back();
        const auto* task = task_of(d.task);
        const std::string_view text = d.kind == DropKind::Result && task != nullptr && !task->question.empty() ? std::string_view{task->question}
                                                                                                               : std::string_view{d.title};
        if (d.kind == DropKind::Alert) { // what a watcher saw: its words, yellow (final review)
            return {.state = BehindState::Alert, .task = 0, .text = d.title};
        }
        if (d.answer && (d.title == "Check done" || d.title == "Check stopped")) { // a threat check's (07a)
            return {.state = BehindState::Answer, .task = d.task, .text = d.text, .key = d.title, .red = d.red, .quiet = d.quiet};
        }
        return {.state = BehindState::Answer, .task = d.task, .text = text};
    }
    // At work: the newest task at it, the others and the watchers on its eye.
    const auto at_work = static_cast<std::size_t>(std::ranges::count_if(m.background, listed));
    for (auto b = m.background.rbegin(); b != m.background.rend(); ++b) {
        if (!listed(*b)) {
            continue;
        }
        t = {.state = BehindState::Working, .task = b->task, .text = b->question, .step = 0, .steps = b->steps.size(),
             .started_at = b->started_at, .progress = b->progress, .others = at_work - 1 + watchers_on};
        const auto run = std::ranges::find(b->steps, events::StepState::Run, &events::Step::state);
        t.step = run == b->steps.end() ? b->steps.size() : static_cast<std::size_t>(run - b->steps.begin()) + 1;
        return t;
    }
    if (watchers_on > 0) {
        const auto first = std::ranges::find_if(m.watchers, &Watcher::on);
        return {.state = BehindState::Watchers, .task = 0, .text = first->title, .others = watchers_on};
    }
    return t;
}

auto presence_of(const Model& m) -> Presence {
    const Behind t = behind_of(m);
    switch (t.state) {
    case BehindState::Decision:
        return {.state = PresenceState::Decision, .task = t.task, .word = "Needs a decision", .line = t.text, .dot = true};
    case BehindState::Alert: // what a watcher saw: held like a decision, its own words
        return {.state = PresenceState::Decision, .task = t.task, .word = "Noticed", .line = t.text, .dot = true};
    case BehindState::Failed:
        return {.state = PresenceState::Failed, .task = t.task, .word = "Failed", .line = t.text, .dot = true};
    case BehindState::Answer: // a threat check has its own word; one stopped part way has no dot
        return {.state = PresenceState::Done, .task = t.task, .word = t.key.empty() ? std::string_view{"Answer ready"} : t.key, .line = t.text,
                .dot = !t.quiet};
    case BehindState::Working:
        return {.state = PresenceState::Working, .task = t.task, .progress = t.progress, .word = "Working", .line = t.text};
    case BehindState::Watchers: // nothing to tell at the edge; the capsule says what watches
        return {.state = PresenceState::Rest, .word = "Watching", .line = t.text};
    case BehindState::None: break;
    }
    return {.state = PresenceState::Rest, .line = "Ask anything"};
}

auto listed(const TaskState& t) -> bool {
    return working(t) || t.permission.has_value() || t.paused;
}

auto active_count(const Model& m) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(m.background, listed) + std::ranges::count_if(m.watchers, &Watcher::on));
}

namespace {

// A background task to the front (its question card, its buttons); a task working in front
// takes its place in the background.
// The front task goes on behind the tab when it works or asks; a question it was asking waits
// in its card, unanswered.
void behind(Model& m) {
    if (m.task == 0 || (!working(m) && !m.permission)) {
        return;
    }
    m.background.push_back(static_cast<const TaskState&>(m));
    if (m.permission) {
        std::erase_if(m.drops, [&](const Drop& d) { return d.task == m.task && d.kind != DropKind::Alert; });
        m.drops.push_back({.task = m.task, .kind = DropKind::Question, .title = "Needs a decision",
                           .text = m.permission->title.empty() ? m.permission->reason : m.permission->title, .actions = {}});
        ++m.drops_made;
        m.drops.back().made = m.drops_made;
    } else if (m.query) { // the model's question waits behind the handle (phase 12 § 4)
        std::erase_if(m.drops, [&](const Drop& d) { return d.task == m.task && d.kind != DropKind::Alert; });
        m.drops.push_back({.task = m.task, .kind = DropKind::Question, .title = "Needs a decision", .text = m.query->question, .actions = {}});
        ++m.drops_made;
        m.drops.back().made = m.drops_made;
    } else if (waits_go_on(m)) { // "Go on?" asked in front: behind the handle it waits for a decision
        std::erase_if(m.drops, [&](const Drop& d) { return d.task == m.task && d.kind != DropKind::Alert; });
        m.drops.push_back({.task = m.task, .kind = DropKind::Question, .title = "Needs a decision", .text = last_paragraph(m.answer),
                           .actions = m.actions});
        ++m.drops_made;
        m.drops.back().made = m.drops_made;
    }
}

auto bring_front(Model& m, std::uint64_t task) -> bool {
    const auto it = std::ranges::find(m.background, task, &TaskState::task);
    if (it == m.background.end()) {
        return false;
    }
    TaskState incoming = std::move(*it);
    m.background.erase(it);
    if (m.task != task && !working(m) && !m.permission && !m.question.empty()) {
        archive(m); // a finished turn in front stays in the session
    } else if (m.task != task) {
        behind(m);
    }
    static_cast<TaskState&>(m) = std::move(incoming);
    m.input = {};
    if (m.permission) {
        m.ticks.assign(m.permission->items.size(), true); // its own choices, all ticked
        m.permanent = false;
        m.folder_tick = false;
        m.all_shown = false;
        m.pick = 0;
    }
    return true;
}

} // namespace

auto drop_press(Model& m, std::uint64_t task, std::string_view action) -> Reaction {
    Reaction r;
    const auto drop = std::ranges::find(m.drops, task, &Drop::task);
    if (drop == m.drops.end()) {
        return r;
    }
    const auto forget = [&] { std::erase_if(m.drops, [&](const Drop& d) { return d.task == task; }); };
    if (action == "copy") { // the first sentence, the sheet closed; it stays unread
        r.clipboard = drop->text;
        return r;
    }
    if (action == "close") {
        forget();
        std::erase_if(m.background, [&](const TaskState& t) { return t.task == task && !working(t); });
        return r;
    }
    if (action == "skip") {
        const auto it = std::ranges::find(m.background, task, &TaskState::task);
        if (it != m.background.end() && it->permission) {
            r.command = events::Answer{task, it->permission->id, events::Choice::Deny};
            it->permission.reset();
            it->activity = Activity::Thinking; // the agent goes on without it
        } else if (it != m.background.end() && waits_go_on(*it)) { // "Go on?": its safe answer is Stop
            r.command = events::Act{task, "stop"};
            it->actions.clear();
        }
        forget();
        return r;
    }
    const bool offered = action == "review" || std::ranges::any_of(drop->actions, [&](const events::Action& a) { return a.id == action; });
    if (!offered || !bring_front(m, task)) {
        return r;
    }
    forget();
    r.open = true;
    if (action != "review") {
        r.command = press(m, action);
    }
    return r;
}

namespace {

// The question a background task asks, if it asks.
auto asked(const Model& m, std::uint64_t task) -> const events::Permission* {
    const auto it = std::ranges::find(m.background, task, &TaskState::task);
    return it != m.background.end() && it->permission ? &*it->permission : nullptr;
}

} // namespace

// A background task stopped at "Go on?".
auto at_go_on(const Model& m, std::uint64_t task) -> bool {
    const auto it = std::ranges::find(m.background, task, &TaskState::task);
    return it != m.background.end() && !it->permission && waits_go_on(*it);
}

auto safe_answer(const Model& m, std::uint64_t task) -> std::string_view {
    const auto* p = asked(m, task);
    if (p == nullptr && at_go_on(m, task)) {
        return "Stop"; // its budget spent: going on is the user's, in the window
    }
    return p == nullptr ? "Skip" : p->picture ? "Don't send" : p->kind == events::Kind::Delete ? "Keep all" : "Skip";
}

auto decision_note(const Model& m, std::uint64_t task) -> std::string_view {
    const auto* p = asked(m, task);
    if (p == nullptr && at_go_on(m, task)) {
        return "Nothing more is done until you decide. I'm paused.";
    }
    return p != nullptr && p->picture                        ? "Nothing is sent yet. I'm paused until you decide."
           : p != nullptr && p->kind == events::Kind::Delete ? "Nothing is deleted yet. I'm paused until you decide."
                                                             : "Nothing is changed yet. I'm paused until you decide.";
}

auto permission_text(const events::Permission& p) -> std::string {
    std::string reason = p.reason; // the policy's phrase ("moves …") as the card's sentence
    if (!reason.empty() && reason[0] >= 'a' && reason[0] <= 'z') {
        reason[0] = static_cast<char>(reason[0] - 'a' + 'A');
    }
    std::string out = "**" + reason + "**\n";
    for (const auto& w : p.what) {
        out += "- " + w + "\n";
    }
    if (!p.preview.empty()) {
        out += p.preview;
        if (out.back() != '\n') {
            out += '\n';
        }
    }
    return out;
}

auto key(Model& m, Key k, bool ctrl, bool shift, bool alt) -> Reaction {
    Reaction r;
    if (m.menu.open && !m.key_mode && !m.permission) { // MODEL_PICKER_SPEC § 8
        const std::size_t rows = menu_rows(m);
        const std::size_t last = rows == 0 ? 0 : rows - 1;
        const auto light = [&](std::size_t i) {
            m.menu.active = std::min(i, last);
            m.menu.by_keys = true;
        };
        constexpr std::size_t kPage = 6; // rows a page: the menu's height shows about so many
        switch (k) {
        case Key::Up: light(m.menu.active == 0 ? 0 : m.menu.active - 1); return r;
        case Key::Down: light(m.menu.active + 1); return r;
        case Key::Home: light(0); return r;
        case Key::End: light(last); return r;
        case Key::PageUp: light(m.menu.active > kPage ? m.menu.active - kPage : 0); return r;
        case Key::PageDown: light(m.menu.active + kPage); return r;
        case Key::Enter: return pick(m, m.menu.active, alt);
        case Key::Escape:
            if (!m.menu.query.text.empty()) {
                m.menu.query = {};
                regroup(m);
                m.menu.active = 0;
            } else {
                close_menu(m);
            }
            return r;
        case Key::Left:
        case Key::Right: {
            ModelChoice c = effective(m);
            c.level = std::clamp(c.level + (k == Key::Right ? 1 : -1), 0, 3);
            return set_choice(m, c, m.next_only.has_value(), m.now);
        }
        case Key::Backspace:
        case Key::Delete:
            if (k == Key::Backspace) {
                ctrl ? backspace_word(m.menu.query) : backspace(m.menu.query);
            } else {
                del(m.menu.query);
            }
            regroup(m);
            m.menu.active = 0;
            return r;
        case Key::Tab: return r;
        }
    }
    if (m.key_mode) {
        switch (k) {
        case Key::Escape:
            m.key = {};
            if (!m.has_key) { // no way past the key: Esc closes the sheet, which opens on the key again
                m.key_error.clear();
                r.close = true;
                return r;
            }
            m.key_mode = false;
            return r;
        case Key::Enter: (void)submit(m); return r;
        case Key::Backspace: ctrl ? backspace_word(m.key) : backspace(m.key); return r;
        case Key::Delete: del(m.key); return r;
        case Key::Left: left(m.key); return r;
        case Key::Right: right(m.key); return r;
        case Key::Home: home(m.key); return r;
        case Key::End: end(m.key); return r;
        default: return r; // no history, no Tab: the key stays in its field
        }
    }
    if (m.private_focus) {
        // Private windows' own field: Enter adds the rule, Esc gives the typing back to the bar.
        switch (k) {
        case Key::Enter: r.command = add_private(m); return r;
        case Key::Escape:
        case Key::Tab: m.private_focus = false; return r;
        case Key::Backspace: ctrl ? backspace_word(m.private_add) : backspace(m.private_add); return r;
        case Key::Delete: del(m.private_add); return r;
        case Key::Left: extend(m.private_add, shift, ctrl ? &word_left : &left); return r;
        case Key::Right: extend(m.private_add, shift, ctrl ? &word_right : &right); return r;
        case Key::Home: extend(m.private_add, shift, &home); return r;
        case Key::End: extend(m.private_add, shift, &end); return r;
        case Key::Up:
        case Key::Down: return r;
        }
    }
    if (m.filter_focus) {
        // The panel's filter: its own editing; Esc and Enter give the focus back to the field.
        switch (k) {
        case Key::Escape:
        case Key::Enter:
        case Key::Tab: m.filter_focus = false; return r;
        case Key::Backspace: ctrl ? backspace_word(m.filter) : backspace(m.filter); return r;
        case Key::Delete: del(m.filter); return r;
        case Key::Left: extend(m.filter, shift, ctrl ? &word_left : &left); return r;
        case Key::Right: extend(m.filter, shift, ctrl ? &word_right : &right); return r;
        case Key::Home: extend(m.filter, shift, &home); return r;
        case Key::End: extend(m.filter, shift, &end); return r;
        case Key::Up:
        case Key::Down: return r;
        }
    }
    if (k == Key::Enter && shift && view(m) != View::Log) { // a new line in the bar's field (phase 10)
        type(m, "\n");
        return r;
    }
    if (k == Key::Escape && editor_shown(m)) { // the editor folds back to the row, its text kept (screen 29)
        m.editor = false;
        return r;
    }
    if (m.permission) {
        // Esc is No; Enter never allows (a click or the focused button does): it sends what the
        // field holds as a new request, which replaces the asking task.
        switch (k) {
        case Key::Escape: r.command = answer(m, events::Choice::Deny); return r;
        case Key::Backspace: ctrl ? backspace_word(m.input) : backspace(m.input); return r;
        case Key::Delete: del(m.input); return r;
        case Key::Left: extend(m.input, shift, ctrl ? &word_left : &left); return r;
        case Key::Right: extend(m.input, shift, ctrl ? &word_right : &right); return r;
        case Key::Home: extend(m.input, shift, &home); return r;
        case Key::End: extend(m.input, shift, &end); return r;
        case Key::Tab:
        case Key::Up:
        case Key::Down: return r;
        case Key::Enter: r.command = submit(m); return r;
        }
    }
    if (view(m) == View::Quick) {
        if (k == Key::Escape) { // "Esc, and the bar is empty again"
            m.quick.reset();
            m.answer.clear();
            m.actions.clear();
            m.activity = Activity::Idle;
            m.input = {};
            return r;
        }
        if (k == Key::Tab) { // "Tab — more": the whole answer, when the agent gave one
            m.quick_more = !m.answer.empty();
            return r;
        }
    }
    if (view(m) == View::Terminal && k == Key::Escape) {
        expand(m, false); // folded back; the task goes on
        return r;
    }
    if (k == Key::Tab && m.activity == Activity::Answering && has_detail(m) && view(m) != View::Quick) {
        expand(m, true); // "Tab — more": the detailed mode
        return r;
    }
    if (view(m) == View::Log) {
        if (k == Key::Escape) {
            close_log(m); // back to the field; the sheet stays
            return r;
        }
        if (k == Key::Enter || k == Key::Up || k == Key::Down || k == Key::Tab) {
            return r; // the field searches: nothing to send, nothing to recall
        }
    }
    if (k == Key::Escape) {
        r.close = true;
        return r;
    }
    switch (k) {
    case Key::Enter: r.command = m.activity == Activity::Thinking || (working(m) && !m.permission) ? refine(m) : submit(m); break;
    case Key::Up: recall(m, true); break;
    case Key::Down: recall(m, false); break;
    case Key::Tab: break;
    case Key::Backspace:
        if (m.input.text.empty() && !m.attachments.empty()) { // nothing typed: the last attachment goes
            m.attachments.pop_back();
            m.attach_note.clear();
            break;
        }
        ctrl ? backspace_word(m.input) : backspace(m.input);
        break;
    case Key::Delete: del(m.input); break;
    case Key::Left: extend(m.input, shift, ctrl ? &word_left : &left); break;
    case Key::Right: extend(m.input, shift, ctrl ? &word_right : &right); break;
    case Key::Home: extend(m.input, shift, &home); break;
    case Key::End: extend(m.input, shift, &end); break;
    case Key::Escape: break;
    }
    return r;
}

auto send_back(Model& m) -> bool {
    if (m.task == 0 || !m.permission) {
        return false;
    }
    behind(m);
    static_cast<TaskState&>(m) = TaskState{};
    m.input = {};
    return true;
}

void open_turn(Model& m, std::size_t turn) {
    if (turn >= m.session.size()) {
        return;
    }
    auto& t = m.session[turn];
    t.collapsed = !t.collapsed;
    if (!t.collapsed && !t.data.empty()) {
        activate(m, t.data.front());
    }
}

void show_set(Model& m, std::uint32_t id) {
    activate(m, id);
    m.activity_task = 0;
}

void space_show(Model& m, std::uint32_t set, std::vector<std::uint32_t> path, bool files) {
    const auto it = std::ranges::find(m.sets, set, &events::DataSet::id);
    if (it == m.sets.end() || !it->space) {
        return;
    }
    *it = space::set_of(it->space, std::move(path), files ? space::View::LargestFiles : space::View::Folders, it->id,
                        std::move(it->snapshot));
}

void toggle_transcription(Model& m) {
    m.transcription = !m.transcription;
}

void show_activity(Model& m, std::uint64_t task) {
    m.activity_task = task;
    m.filter_focus = false;
    m.private_focus = false;
}

void show_page(Model& m, std::uint32_t page) {
    for (auto id = m.data.rbegin(); id != m.data.rend(); ++id) {
        const auto set = std::ranges::find(m.sets, *id, &events::DataSet::id);
        if (set != m.sets.end() && set->kind == events::SetKind::Document) {
            show_set(m, set->id);
            m.doc_page = page;
            return;
        }
    }
}

void hide_panel(Model& m) {
    m.active_set = 0;
    m.activity_task = 0;
    m.filter_focus = false;
    m.private_focus = false;
}

void focus_filter(Model& m, bool on) {
    m.filter_focus = on;
}

void focus_private(Model& m, bool on) {
    m.private_focus = on;
    if (on) {
        m.filter_focus = false;
    }
}

auto add_private(Model& m) -> std::optional<events::Command> {
    auto text = m.private_add.text;
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return std::nullopt;
    }
    text.erase(0, first);
    m.private_add = {};
    // App: the program's file name; Title and Site: words in the window's title (a browser's names its site).
    return m.private_kind == 0 ? events::AddPrivate{.program = std::move(text), .words = {}}
                               : events::AddPrivate{.program = {}, .words = std::move(text)};
}

auto new_chat(Model& m) -> std::optional<events::Command> {
    std::optional<events::Command> cancel;
    if (working(m)) {
        cancel = events::Cancel{m.task};
    }
    const std::uint64_t last = std::max(m.last_task, m.task);
    static_cast<TaskState&>(m) = TaskState{};
    m.last_task = last; // whatever the stopped task still sends is ignored
    m.input = {};
    m.session.clear();
    forget_sets(m); // a background task's tables stay for its card
    m.active_set = 0;
    m.fresh_chat = true;
    m.activity_task = 0;
    m.filter = {};
    m.filter_focus = false;
    m.private_focus = false;
    return cancel;
}

} // namespace app::sheet
