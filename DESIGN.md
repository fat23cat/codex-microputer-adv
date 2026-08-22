# Design System

## Theme

A full-bleed six-channel instrument panel on a 240x135 display. Flat colour,
square geometry and one-pixel separators make the display feel physically
continuous with the Cardputer keyboard.

## Color

- Bone `#F4F2EC`: empty surface and light foreground.
- Ink `#17150F`: error surface and dark foreground.
- Blue `#1B4FD0`: running.
- Vermilion `#E2451E`: action required and error accent.
- Leaf `#4CB949`: completed and not yet viewed.
- Light neutral `#CED0CB`: completed and viewed.
- Pale `#DEDBD1`: idle or unbound.
- Ordinal `#ABA89D`: row numbers on the local screens. One step quieter than
  label type and still legible at the 10 % dim level, which the hairline rule
  is not.

## Typography

Task numerals use Micro5, embedded as monochrome masks under OFL-1.1.
Small system labels use the existing compact M5 bitmap fonts. Numerals share a
single optical size and baseline at rest.

## Layout

Six full-height vertical slots span the display. Adjacent slots are separated
by exactly one pixel with no external gutter. A compact square-ended selection
bar marks the active slot below the numeral.

The local screens are one column of numbered rows: a two-digit ordinal, a
tracked label, and a right-aligned value. Ordinals are counted from 01 for the
same reason the deck is, so the physical keys and every listed control belong to
one numbered system.

## Motion

Selection is a short, independent vertical reveal of the bottom bar. A status
event changes colour, expands the originating slot to the full display, centres
its numeral, holds for two seconds, then reverses. A newer event for the active
slot replaces it; events for different slots play in arrival order.

A key answers on contact: press feedback reaches full value on the first frame
and eases out, rather than ramping up and back down.

Every local screen moves the same way. One selection plate travels on a spring
while the rows stay still, and the focused row is drawn as the same row seen
through the plate, so its inversion arrives exactly where the plate is. The
chime grid uses two springs so a diagonal move reads as one object crossing the
panel. A page opens with its plate already in place; only moving within a page
is a gesture.

## Sound

Status cues carry their meaning in the interval, not in the volume. A cue may
transpose its key between plays; its rhythm, envelope and consonance never
change.

Cues sit in the register the speaker can actually radiate, and open by rising
into their first note rather than starting on it. Nothing in a status score is
allowed to be bright for its own sake: this transducer turns energy above
roughly 2 kHz into hardness, so a figure that needs to carry does it by
interval and by envelope, never by climbing.

What separates an instrument from a beep is the envelope, not the pitch. A
note is blown into rather than switched on, its upper partials die before its
fundamental does, and the breath that excited it is audible before it settles.
A hard attack on a nearly pure tone is a beep however it is tuned, and a voice
that is cut off mid-decay puts a step in the output exactly where the ear is
listening hardest.

Weight belongs on the octave, not on the fundamental. Below roughly 300 Hz this
transducer radiates very little and answers level with cone excursion, so a low
note carried by its fundamental is inaudible and rattling at the same time.
Handing the ear the pair instead lets it hear the lower note while the speaker
moves half as far. Anything that shapes the sound over time must be continuous:
a value held flat between updates is a staircase, and a staircase repeats at a
kilohertz, which is exactly where this speaker is most efficient and where a low
chord has nothing of its own to mask it.

Everything the ear can hear moving belongs to one clock. Within a cue, the rate
of a pulse, a repeat and a chord change are powers of two off a single phase, so
they read as one instrument keeping time rather than as parts drifting past each
other.

A score is synthesised on demand, and it must finish inside the debounce window
that precedes the takeover it accompanies — otherwise the sound the animation
was built around arrives after the animation. That budget is what the scores
are written against: no library maths on the per-sample path, every voice
skipped outside the window where it is audible, and slow shaping voices held
rather than recomputed. Uniqueness comes from the key and the interval chosen
per play, which cost nothing, never from rendering more.

Level is not loudness. This transducer radiates almost nothing below a few
hundred hertz, and the ear is least sensitive exactly where it gives up, so two
cues written at the same numbers arrive at different volumes if they sit in
different registers — which is what made the input request the quietest thing
on the device the moment it moved down a fifth. Every voice in every score is
therefore scaled by where it sits: flat above 760 Hz, rising about 4.5 dB per
octave below it, and stopping at 280 Hz, under which there is nothing to
recover and more level buys only a rattle. The correction is per voice, not per
cue — a bed two octaves under its own gesture has the same problem — and it is
computed before the sample loop, because it is a std::pow.

Once voices are corrected, no score can be written against a fixed output
scale: what a cue peaks at depends on which notes it is playing. Each one is
rendered at a provisional scale and trimmed to its target peak once the last
sample is in. That is also the only way cues end up equally loud rather than
equally scaled, and it puts the hierarchy between them in one place: a request
and a result own the room, a fault matches them, work in progress sits under
them, a release is the quietest thing the device does.

- Running: an upward chirp over a quiet motor pulse. Continuous.
- Done: three staggered bell partials resolving into a major consonance.
- Attention: a question, and audibly not a result. It shares the grammar of
  every other score — a struck gesture over a quiet sustained bed with a soft
  tail — but nothing else with the completion bell, because a task finishing
  and a task asking are opposite news and telling them apart must not depend
  on noticing which one resolves. Three things separate them, and none is the
  melody. Timbre: the bell carries its second partial and rings; the ask
  carries its third and no octave at all, odd harmonics only, hollow and reedy
  where the bell is glassy — a stopped pipe against struck metal. Register:
  the ask speaks a fifth above its chord, around 260–400 Hz, where completion
  never goes; a question is asked at the bottom of the voice and a result
  announced at the top of it. Rhythm: the bell rolls three partials evenly;
  the ask is speech — two notes close together, then a gap two and a half
  times as long before the one it settles on.
  It is also the only cue that remembers the last time it spoke. A run of
  questions is a phrase, not one figure returned to: each ask takes the next
  step of a four-chord progression in one key, so the bed moves underneath and
  the notes are drawn from whichever chord is standing. The arc peaks on the
  third step, where a single quiet fifth above the phrase's top note lights
  it, and leans back on the fourth, which wants the first again — the cycle
  closes without resolving, so a long series keeps going round and answering
  four things in a row is heard as a line arriving somewhere. The key is
  chosen once for a series and held; transposing every ask, the way every
  other cue transposes every play, is what would stop the progression from
  being one. A new key is rolled only after half a minute of quiet.
  Each step lifts to a peak and settles a step below it — an intonation
  contour, not a climb. Settling is not descending: the fault cue falls
  through its start and keeps going, while the ask comes to rest above where
  it began, on a chord tone that is never the root, which is what leaves it
  open. Notes get quieter as the phrase goes on, and the one it settles on is
  quietest, longest and slowest to open — arrived at rather than struck. No
  note bends its pitch: a rising leading tone is the shape of a spoken
  question, but on a pure tone through a transducer this small it is only a
  whine. Under it is one bare open fourth on the shared hold envelope, at the
  weight every other score gives its bed — a fourth rather than a fifth
  because a bed fifth would sit exactly on the phrase's first note and fight
  it for the same air. It never gates, pulses or repeats: a cue that keeps
  restating itself at a fixed rate is what turns a request into nagging.
- Error: a descending, roughened interval that refuses to resolve. This is the
  only cue permitted to sound wrong.
- Idle: a soft downward release, deliberately the least prominent.

## Components

- Task slot: semantic fill, fixed numeral, optional error mark.
- Selection bar: foreground-coloured, square, independent of numeral geometry.
- Status takeover: spatial expansion rooted in the changed task slot.
- Link dot: minimal connectivity signal in the upper-right corner.
- Selection plate: one sprung ink rectangle per list; inverted content is
  clipped to it rather than coloured per row.
- Segment meter: a level drawn as discrete blocks, one block per step of the
  control. Empty steps keep a two-pixel seat so the meter always states its
  full range. The arriving block mixes in with the spring instead of popping.
- Segment selector: the same blocks used for one-of-N, with a single filled
  position and seats for the alternatives.
- Corner annunciator: exceptional deck chrome is flush with the panel edges and
  measured in whole task columns. The weak-link notice is two columns wide in
  the top-right, and it is built as an instrument panel rather than as a
  warning label. A dark plate carrying a word in reverse is a sticker applied
  over the deck; an inset of the same paper the interface is printed on,
  delimited by a one-pixel rule and bleeding off the screen edge, is part of
  the panel. It holds a short technical code for what is weak at the left and
  the level itself at the right, with air between them instead of a filled
  bar. The level is three equal dots, not an ascending staircase: equal marks
  are a scale being read, a staircase is a picture of a signal. One dot lit
  warm and two left in spent chrome says one of three without a word for it,
  and the spent dots stay drawn — a scale missing two positions reads as a
  two-position scale. A critical battery readout steps aside for the plate
  rather than overprinting it. Neither is drawn while the link is healthy and
  the battery is not.
- Build stamp: the running firmware version, set in the splash's top-right
  trim with the registration marks — right-aligned to the inner edge of the
  corner mark, on the same rule, tracked tight and mixed most of the way back
  to the paper. It is reference, not identity: it should be findable, not
  read, so it never joins the wordmark on the centre line. The pixel face has
  one size, so "smaller" is tighter and fainter rather than a second font. It
  appears only on the splash and is never part of the deck.
