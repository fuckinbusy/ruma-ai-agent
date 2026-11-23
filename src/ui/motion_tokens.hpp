#pragma once

// ui::motion — every number of the agent's motion, here and nowhere else (motion rework:
// one group of named constants, so a speed is one edit). The source is
// the designer's design/ui_motion/MOTION_SPEC.md ("MS"): its tokens (§ 3) and each scene's starts,
// targets and amplitudes (§ 6, § 7). scripts/check.ps1 fails on motion numbers anywhere else.

#include <chrono>
#include <cstddef>

namespace ui::motion {

// A spring: `response` — seconds to come within ~1 % of the way (critically damped), `damping` —
// the ratio ζ (1: no overshoot).
struct MotionToken {
    float response = 0.36f;
    float damping = 1.0f;
};

// MS § 3. Leaving is a token faster than arriving (MS § 2.4).
inline constexpr MotionToken kMicro{0.14f, 1.0f};  // a press, small things leaving, hovering a button
inline constexpr MotionToken kQuick{0.24f, 1.0f};  // opacity, text, colour, small things, row highlight
inline constexpr MotionToken kBase{0.36f, 1.0f};   // shifts, scale, the tab underline, chevrons, the bar's curtain
inline constexpr MotionToken kSmooth{0.50f, 1.0f}; // heights, expanding, flights, scrolling, progress, FLIP rows
inline constexpr MotionToken kGentle{0.70f, 1.0f}; // the dim around the window, background layers
inline constexpr MotionToken kMorph{0.60f, 0.86f}; // only window <-> handle: ~1 % overshoot
inline constexpr MotionToken kOrb{1.10f, 1.0f};    // the orb's shader parameters (idle <-> thinking)

// MS § 5.1: a frame's time step never exceeds this; after a stall motion goes on from where it was.
inline constexpr float kMaxFrameDt = 1.0f / 30.0f;

// MS § 2.6: a wave staggers at most this many items; the rest start with the tenth.
inline constexpr std::size_t kStaggerMaxItems = 10;

// A height's spring rests on its target within 0.1 % of its range (ui::motion::Spring::range): the
// height it moves through, at least this many pixels.
inline constexpr float kHeightRange = 100.0f;

// MS § 6, scene 01 "Appearing on the hotkey": the curtain opens 0 -> the bar on kBase from +0;
// the content comes down kSummonLift px on kSmooth from +0; the orb from kOrbAt (opacity kQuick,
// scale kOrbScaleFrom -> 1 kBase); the hint from kHintAt (opacity kQuick, kHintShift px -> 0 kBase);
// the model's pill and the button from kRowAt (opacity kQuick, the button's scale kButtonScaleFrom
// -> 1 kBase).
namespace summon {
inline constexpr std::chrono::milliseconds kOrbAt{30};
inline constexpr std::chrono::milliseconds kHintAt{70};
inline constexpr std::chrono::milliseconds kRowAt{110};
inline constexpr float kContentLift = -16.0f;
inline constexpr float kOrbScaleFrom = 0.72f;
inline constexpr float kHintShift = 10.0f;
inline constexpr float kButtonScaleFrom = 0.86f;
} // namespace summon

// MS § 6, scenes 02 and 03 (Esc). The bar alone (02): the hint, the pill and the button go on kMicro
// from +0, the orb's opacity kQuick and its scale -> kOrbScaleTo kBase from +0, the curtain -> 0 and
// the content -> kBarContentDrop px on kBase from kCurtainAt. Expanded (03): the columns go on kQuick
// from +0, the bar's row on kQuick from kRowAt, the curtain -> 0 and the content -> kExpandedContentDrop
// on kSmooth from kCurtainAt — one spring, no "first to the bar" phase.
// MS § 6, scene 15: Esc during the summon turns everything round at once (no kCurtainAt): the curtain
// and the content (-> kBarContentDrop) on kBase, the orb's opacity kQuick, the hint and the row kMicro.
namespace dismiss {
inline constexpr std::chrono::milliseconds kCurtainAt{20};
inline constexpr std::chrono::milliseconds kRowAt{40};
inline constexpr float kOrbScaleTo = 0.8f;
inline constexpr float kBarContentDrop = -12.0f;
inline constexpr float kExpandedContentDrop = -28.0f;
} // namespace dismiss

// The hidden agent's sign (UI update, stage 3; design/ui_presence, variant A · Handle): a bar at the
// top edge, kTop px under it — kWidth x kHeight at rest, kWorkWidth wide while the agent works, its
// corners kRadius; a halo kHalo px round it; a dot kDot px, kDotGap from its right end. Under the
// mouse (a zone kZoneWidth x kZoneHeight) it stretches into a capsule kCapsuleHeight tall and goes
// back kCollapseAfter after the mouse has left. Its motion (README's table): handle <-> capsule on
// kMorph, the capsule's words on kQuick once the shape is past kWordsAfterShare of its way; the
// width at rest <-> at work on kBase; the progress fill on kSmooth, never back; the dot from
// kDotFrom of its size on kBase, its opacity on kQuick; the bar's colour on kSmooth.
namespace sign {
inline constexpr float kWidth = 44.0f;
inline constexpr float kWorkWidth = 72.0f;
inline constexpr float kHeight = 4.0f;
inline constexpr float kRadius = 3.0f;
inline constexpr float kTop = 3.0f;
inline constexpr float kHalo = 1.5f;
inline constexpr float kDot = 6.0f;
inline constexpr float kDotGap = 5.0f;
inline constexpr float kZoneWidth = 120.0f;
inline constexpr float kZoneHeight = 24.0f;
inline constexpr float kCapsuleHeight = 38.0f;
// The window <-> the handle as one shape on kMorph (scenes::Fold; the motion rework's scenes 12, 13).
// Into it: the window's content goes on kQuick from +0, the shape from kFoldShapeAt. Out of it: the
// shape from kUnfoldShapeAt, the bar's row on kBase from kUnfoldBarAt, what is under it from
// kUnfoldColumnsAt, coming down kColumnsLift px on kSmooth.
inline constexpr std::chrono::milliseconds kFoldShapeAt{30};
inline constexpr std::chrono::milliseconds kUnfoldShapeAt{20};
inline constexpr float kColumnsLift = -10.0f;
inline constexpr std::chrono::milliseconds kUnfoldBarAt{120};
inline constexpr std::chrono::milliseconds kUnfoldColumnsAt{160};
inline constexpr float kCapsuleMin = 160.0f; // its width fits its words, within these
inline constexpr float kCapsuleMost = 460.0f;
inline constexpr std::chrono::milliseconds kCollapseAfter{600};
inline constexpr float kWordsAfterShare = 0.5f;
inline constexpr float kDotFrom = 0.6f;
} // namespace sign

// MS § 6, scene 05 "Send" (Enter at +0; the tracks its table leaves out are the reference's,
// agent-motion.html 05_send). The button goes down to kPress on kMicro and is caught at kPressBackAt
// back to 1 on kBase; the arrow goes on kMicro from kArrowAt; the request's text flies from the field
// into its bubble (to kFlyScale of its size) on kSmooth from kFlyAt, the flying layer giving way to
// the bubble's own text on kQuick from kFlyOutAt; the window grows on kSmooth from kGrowAt; "stop"
// comes on kBase from kStopAt (its scale from kStopScaleFrom), its ring on kSmooth from kRingAt; the
// rail on kSmooth and the chip on kQuick (kChipShift px on kBase) from kRailAt; the new hint (kQuick)
// and the column's head (kBase) from kHintAt; the bubble's ground and the names on kQuick from
// kBubbleAt; the data panel on kBase coming kDataDrop px on kSmooth from kDataAt; the "Working"
// block opens on kSmooth from kWorkingAt.
namespace send {
inline constexpr std::chrono::milliseconds kArrowAt{30};
inline constexpr std::chrono::milliseconds kFlyAt{40};
inline constexpr std::chrono::milliseconds kGrowAt{60};
inline constexpr std::chrono::milliseconds kPressBackAt{80};
inline constexpr std::chrono::milliseconds kStopAt{80};
inline constexpr std::chrono::milliseconds kRailAt{160};
inline constexpr std::chrono::milliseconds kRingAt{180};
inline constexpr std::chrono::milliseconds kHintAt{220};
inline constexpr std::chrono::milliseconds kBubbleAt{300};
inline constexpr std::chrono::milliseconds kDataAt{400};
inline constexpr std::chrono::milliseconds kFlyOutAt{420};
inline constexpr std::chrono::milliseconds kWorkingAt{460};
inline constexpr float kPress = 0.92f;
inline constexpr float kFlyScale = 0.66f;
inline constexpr float kStopScaleFrom = 0.4f;
inline constexpr float kChipShift = 8.0f;
inline constexpr float kDataDrop = 10.0f;
} // namespace send

// MS § 6, scene 06 "Steps running". A step done: its spinner goes as its circle comes on kBase
// (from kCircleFrom of its size), its check draws itself on kSmooth from kCheckAt after; the next
// step wakes from its dot on kQuick when it runs. The work done: the steps' list goes on kMicro, the
// block folds to its line on kSmooth from kFoldAt, "Working" goes on kQuick from kFoldOldWordsAt and
// the folded line's words come on kBase from kFoldWordsAt (the reference's hdA, hdB; all counted from
// the list's start).
namespace steps {
inline constexpr std::chrono::milliseconds kCheckAt{40};
inline constexpr std::chrono::milliseconds kFoldAt{40};
inline constexpr std::chrono::milliseconds kFoldOldWordsAt{50};
inline constexpr std::chrono::milliseconds kFoldWordsAt{90};
inline constexpr float kCircleFrom = 0.6f;
// Where a kBase spring is kCheckAt after it starts (1 - (1 + wt)·e^-wt, w = 2π / .36, t = .04): a
// widget with no clock of its own (ui::work's badge) starts the check when its circle is this far.
inline constexpr float kCheckAfterShare = 0.155f;
} // namespace steps

// MS § 6 "Streaming", scene 07 "The answer types out smoothly". The text comes out at
// clamp(backlog · kRatePerBacklog, kRateMin, kRateMax) characters a second, whole words only; each
// character comes in over kCharIn (smoothstep). The paragraph grows by its lines on kQuick; the
// scroll follows on kSmooth to kFollowTail px under the line, only while the user is at the bottom.
// The status turns on kBase kStatusAt after the last character; the cards and hints open on kSmooth
// (opacity kBase, kCardsOpacityAt later) kCardsAt after it.
namespace stream {
inline constexpr float kRatePerBacklog = 6.0f;
inline constexpr float kRateMin = 45.0f;
inline constexpr float kRateMax = 520.0f;
inline constexpr std::chrono::milliseconds kCharIn{160};
inline constexpr float kFollowTail = 24.0f;
inline constexpr std::chrono::milliseconds kStatusAt{60};
inline constexpr std::chrono::milliseconds kCardsAt{220};
inline constexpr std::chrono::milliseconds kCardsOpacityAt{60};
inline constexpr std::chrono::milliseconds kChipStagger{30}; // § 7: the chips after the answer, one after another
} // namespace stream

// MS § 6, scene 08 "Table rows and re-sorting". Rows of one arrival come kStagger apart
// (kStaggerMaxItems at most, the rest with the last): opacity on kQuick, kRowShift px down to their
// place on kBase; a size's bar grows on kSmooth kBarAt after its row; skeleton rows wait where data
// will come (their light crosses a bar in kSkeletonSweep) and go on kQuick; sorted again, each row
// goes to its new place on kSmooth (FLIP); the sort's arrow on kQuick. A row older than kRowYoung
// stands as it is (one scrolled into view later does not come in again).
namespace table {
inline constexpr std::chrono::milliseconds kStagger{35};
inline constexpr std::chrono::milliseconds kBarAt{60};
inline constexpr std::chrono::milliseconds kRowYoung{1000};
inline constexpr std::chrono::milliseconds kSkeletonSweep{1200};
inline constexpr float kRowShift = 6.0f;
// § 7 "Forget": a row leaving goes on kMicro; its room closes on kSmooth from kLeaveCloseAt.
inline constexpr std::chrono::milliseconds kLeaveCloseAt{140};
} // namespace table

// MS § 6, scene 09 "Tabs and data cards". The underline goes under the new tab — place and
// width on kBase; the tabs' words turn on kQuick; the old content goes kAside px aside on kBase, its
// opacity on kQuick; the new one comes from the other side on kSmooth, its opacity on kBase, kInAt
// after the click.
namespace tabs {
inline constexpr std::chrono::milliseconds kInAt{70};
inline constexpr float kAside = 14.0f;
} // namespace tabs

// § 7 "Data panel". The disk's bar: its segments' widths on kSmooth, each kSegmentStagger after
// the one before. A folder opened or left: the old list goes on kMicro, kStepAside px aside on kBase;
// the new one comes on kQuick from kStepInAt, from the other side on kSmooth; the way's new crumb on
// kQuick. A row's graph slides by its step over kSampleEvery (a new point does not jump in). A
// tile's number counts on kSmooth.
namespace panel {
inline constexpr std::chrono::milliseconds kSegmentStagger{30};
inline constexpr std::chrono::milliseconds kStepInAt{60};
inline constexpr std::chrono::milliseconds kSampleEvery{1000};
inline constexpr float kStepAside = 16.0f;
} // namespace panel

// MS 10: something opening under its row. Opening: the room on kSmooth, the chevron on kBase, the
// body on kBase from kBodyInAt, down from kBodyLift px above on kSmooth. Closing: the body on kMicro
// (kBodyDrop px up), the chevron back from kChevronBackAt, the room on kSmooth from kRoomCloseAt.
namespace accordion {
inline constexpr std::chrono::milliseconds kBodyInAt{80};
inline constexpr std::chrono::milliseconds kChevronBackAt{30};
inline constexpr std::chrono::milliseconds kRoomCloseAt{40};
inline constexpr float kBodyLift = 6.0f;
inline constexpr float kBodyDrop = 4.0f;
} // namespace accordion

// MS 14: the small answers. A hover on kQuick both ways; a press down to kPress of its size on
// kMicro and back on kBase — one scale for every button (a card, wide, to kCardPress); a table
// row's accent bar from 0 to kAccentBar px on kBase; the keyboard's focus ring on kBase, from
// kFocusFrom of its size.
namespace micro {
inline constexpr float kPress = 0.96f;
inline constexpr float kCardPress = 0.98f;
inline constexpr float kAccentBar = 2.0f;
inline constexpr float kFocusFrom = 1.06f;
inline constexpr float kPopupLift = 4.0f; // a list opening over the rest: from this far above, on kBase
} // namespace micro

// MS § 7 "Installer" and the map's unnamed rows of it. The appearance: the orb from kOrbFrom of
// its size on kSmooth (never from nothing), shown on kQuick; two rings (kWave1At, kWave2At) growing
// on kSmooth and going on kGentle; the membrane from kMembraneAt — its width, height and radius on
// kMorph from a round seed kSeed px across, shown on kQuick — and the orb to its place with it; the
// membrane's chrome cooling on kGentle from kChromeAt, the light on its edge on kQuick until
// kRimOutAt; the content from kContentAt, its kContentParts parts kContentStagger apart, each on
// kQuick, up from kContentLift px on kBase. The launch: the content goes on kQuick; from kShapeAt
// the window becomes the agent's bar and the orb flies to its place in it, both on kMorph.
namespace installer {
inline constexpr std::chrono::milliseconds kWave1At{250};
inline constexpr std::chrono::milliseconds kWave2At{550};
inline constexpr std::chrono::milliseconds kMembraneAt{550};
inline constexpr std::chrono::milliseconds kChromeAt{650};
inline constexpr std::chrono::milliseconds kRimOutAt{1000};
inline constexpr std::chrono::milliseconds kContentAt{950};
inline constexpr std::chrono::milliseconds kContentStagger{55};
inline constexpr std::chrono::milliseconds kShapeAt{80};
inline constexpr float kOrbFrom = 0.6f;
inline constexpr float kSeed = 150.0f;
inline constexpr float kContentLift = 10.0f;
inline constexpr std::size_t kContentParts = 6;
// Its screens (the map's unnamed rows): the one that goes, on kMicro; the one that comes, on kQuick
// from kScreenInAt, up from kScreenLift px on kBase. The progress ring and its percent on kSmooth.
// A step's chip: its light on kQuick, its done mark from kDoneFrom of its size on kBase.
inline constexpr std::chrono::milliseconds kScreenInAt{60};
inline constexpr float kScreenLift = 8.0f;
inline constexpr float kDoneFrom = 0.6f;
} // namespace installer

// A view's layer changing (not named in MS; MS § 1 row 4): the one that goes, on kQuick; the one
// that comes, on kBase from kInAt — when the first is nearly gone, so their words do not lie on
// each other.
namespace layers {
inline constexpr std::chrono::milliseconds kInAt{120};
} // namespace layers

// § 7 "Spinner": everything that turns — a spinner, the ring round Stop — at one steady speed,
// kTurn a turn.
namespace spin {
inline constexpr std::chrono::milliseconds kTurn{900};
} // namespace spin

// The field's caret: on half of kBlink, off the other (steps, not motion).
namespace caret {
inline constexpr std::chrono::milliseconds kBlink{1000};
} // namespace caret

// § 7 "The 'Looked at' line": it comes before the answer — its room on kSmooth, its words on kQuick
// from kWordsAt, up from kLift px on kBase, its thumbnail on kQuick from kThumbAt.
namespace looked {
inline constexpr std::chrono::milliseconds kWordsAt{60};
inline constexpr std::chrono::milliseconds kThumbAt{120};
inline constexpr float kLift = 4.0f;
} // namespace looked

// § 7 "Translation": a word's, a phrase's or a sentence's card opens as an answer does — its room on
// kSmooth, its blocks on kQuick one kBlockStagger after another, each up from kLift px on kBase.
namespace translate {
inline constexpr std::chrono::milliseconds kBlockStagger{40};
inline constexpr float kLift = 8.0f;
} // namespace translate

// MS 11: a question asked and answered. Asked: the screen around dims on kGentle; the card's room
// grows on kSmooth from kCardAt; its contents come on kBase from kContentsAt, up from kLift px.
// Answered: the contents go on kQuick; from kShrinkAt the card shrinks on kSmooth to the summary
// row (kSummaryRow px) and turns from yellow to green; the dim goes on kGentle from kDimOutAt; the
// objects' rows go on kQuick from kRowsOutAt, each kRowStagger after the one before, and their room
// closes on kSmooth from kRowsCloseAt; the summary's words come on kBase from kSummaryAt.
namespace approval {
inline constexpr std::chrono::milliseconds kCardAt{40};
inline constexpr std::chrono::milliseconds kContentsAt{120};
inline constexpr std::chrono::milliseconds kShrinkAt{40};
inline constexpr std::chrono::milliseconds kDimOutAt{50};
inline constexpr std::chrono::milliseconds kRowsOutAt{110};
inline constexpr std::chrono::milliseconds kRowsCloseAt{140};
inline constexpr std::chrono::milliseconds kRowStagger{30};
inline constexpr std::chrono::milliseconds kSummaryAt{210};
inline constexpr float kLift = 8.0f;
inline constexpr float kSummaryRow = 52.0f;
} // namespace approval

// § 7 "Threat check · Verdict": the block from kAt after the report is there — its height on
// kSmooth, its contents on kQuick from kContentsAt after that, up from kLift px; its counts from 0
// on kSmooth, each kCountStagger after the one before. A stage's finding mark: .9 → 1 on kBase,
// shown on kQuick.
namespace verdict {
inline constexpr std::chrono::milliseconds kAt{150};
inline constexpr std::chrono::milliseconds kContentsAt{80};
inline constexpr std::chrono::milliseconds kCountStagger{60};
inline constexpr float kLift = 6.0f;
inline constexpr float kMarkFrom = 0.9f;
} // namespace verdict

// § 7 "Rules and memory": a line just remembered is lit green for kLit, then goes pale on kGentle;
// forgotten, it goes as a table's row does (table::kLeaveCloseAt).
namespace memory {
inline constexpr std::chrono::milliseconds kLit{4000};
} // namespace memory

// MS 04 and § 7 "Input row": the row a line more or less on kQuick, emptied back on kSmooth; the
// selection on kMicro both ways; the text cleared goes on kMicro, the hint comes back on kQuick from
// kHintBackAt.
namespace field {
inline constexpr std::chrono::milliseconds kHintBackAt{60};
// An attachment's chip: its width 0 -> its own on kSmooth, its content on kQuick from kChipContentAt;
// removed, its content on kMicro, then (from kChipWidthBackAt) its width -> 0 on kSmooth.
inline constexpr std::chrono::milliseconds kChipContentAt{60};
inline constexpr std::chrono::milliseconds kChipWidthBackAt{140};
} // namespace field

// § 7 "Input row", the model and thinking: the chip's dot and name on kQuick, its width on kBase;
// thinking up — its bars one by one kBarUpStep apart, each on kBase; down — from the top, kBarDownStep
// apart, on kQuick. The hint under the orb: in on kQuick coming kHintRise px on kBase, held kHintHold,
// out on kMicro (kHintOut: the room it keeps meanwhile).
namespace model {
inline constexpr std::chrono::milliseconds kBarUpStep{90};
inline constexpr std::chrono::milliseconds kBarDownStep{60};
inline constexpr std::chrono::milliseconds kHintHold{1200};
inline constexpr std::chrono::milliseconds kHintOut{250};
inline constexpr float kHintRise = 6.0f;
// The picker opening from the bar (design/ui_model, screen 3): its cards come with the glass, the
// parts under them on kQuick, kPartStagger one after another — they stand still.
inline constexpr std::chrono::milliseconds kPartStagger{40};
} // namespace model

// § 7 "Input row", the tooltip at a button: after kDelay of hover; in on kQuick coming kRise px
// on kBase; out on kMicro; to a neighbour (one shown, or gone less than kNeighbourFor ago) at once,
// its place moving there on kBase.
namespace tooltip {
inline constexpr std::chrono::milliseconds kDelay{500};
inline constexpr std::chrono::milliseconds kNeighbourFor{300};
inline constexpr float kRise = 4.0f;
} // namespace tooltip

// The orb (MS § 4 "Orb", the mockup's ChromeOrb). Its shader's phase grows by
// (kPhaseBase + speed · kPhasePerSpeed) a second; its mood's spike, speed and hue move on kOrb.
namespace orb {
inline constexpr float kPhaseBase = 0.35f;
inline constexpr float kPhasePerSpeed = 0.9f;
// A streamed token's pulse raises the energy's target by this much, never past kPulseMost; the
// target fades as e^(-kPulseFade·t) and the energy follows it on kQuick (no step: MS § 1).
inline constexpr float kPulseEnergy = 0.3f;
inline constexpr float kPulseMost = 1.2f;
inline constexpr float kPulseFade = 5.0f;
// MS § 7 "Model chip": a model change sends one ring from the orb's size to kRingTo of it on
// kSmooth, its alpha from kRingAlpha to 0 on kGentle; the material and the glow move on kOrb. The
// drops of thinking take the chip's bars' turns (model::kBarUpStep, kBarDownStep).
inline constexpr float kRingTo = 1.6f;
inline constexpr float kRingAlpha = 0.6f;
} // namespace orb

// How long things stay (the sheet's timers, the installer's light on its edge): time that is part of the
// motion, so it lives here too — not in the code that waits.
namespace hold {
inline constexpr std::chrono::milliseconds kDoneBar{4000};  // the done bar, before it goes back to the field
inline constexpr std::chrono::milliseconds kTurnLive{1000}; // a turn is followed this long after its last text or work
inline constexpr std::chrono::milliseconds kCopied{1500};   // "Copied", before "Copy path" again
inline constexpr std::chrono::milliseconds kPasted{2000};   // "Ctrl+V" over a pasted picture
inline constexpr std::chrono::milliseconds kRowsGoing{700}; // an answered question's rows, to leave (MS 11)
} // namespace hold

// The installer's launch and appearance: how long the light runs along the bar's edge before it goes, when
// the orb's swell begins to ebb, and how fast the light turns round the edge (degrees a second).
namespace installer_hold {
inline constexpr std::chrono::milliseconds kRimRun{350};
inline constexpr std::chrono::milliseconds kSwellEbb{200};
inline constexpr float kRimTurnAppear = 540.0f;
inline constexpr float kRimTurnLaunch = 500.0f;
} // namespace installer_hold

} // namespace ui::motion
