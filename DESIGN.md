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

## Typography

Task numerals use Micro5, embedded as monochrome masks under OFL-1.1.
Small system labels use the existing compact M5 bitmap fonts. Numerals share a
single optical size and baseline at rest.

## Layout

Six full-height vertical slots span the display. Adjacent slots are separated
by exactly one pixel with no external gutter. A compact square-ended selection
bar marks the active slot below the numeral.

## Motion

Selection is a short, independent vertical reveal of the bottom bar. A status
event changes colour, expands the originating slot to the full display, centres
its numeral, holds for two seconds, then reverses. A newer event for the active
slot replaces it; events for different slots play in arrival order.

## Components

- Task slot: semantic fill, fixed numeral, optional error mark.
- Selection bar: foreground-coloured, square, independent of numeral geometry.
- Status takeover: spatial expansion rooted in the changed task slot.
- Link dot: minimal connectivity signal in the upper-right corner.
