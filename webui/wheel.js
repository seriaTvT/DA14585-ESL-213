/*
 * wheel.js - the mouse wheel over a range input.
 *
 * Sliders here are adjusted while watching the preview, and reaching for the
 * handle to nudge a threshold by one is the slowest way to do the commonest
 * thing. The wheel is the obvious gesture and browsers do not give it to
 * range inputs.
 *
 * The stepping is separated from the event plumbing because it is where the
 * quiet mistakes live: a step that does not divide the range leaves a value
 * the slider cannot show, and rounding drift accumulates over a scroll.
 */

/** Read min/max/step off a range input, with the HTML defaults. */
export function bounds(input) {
  const min = input.min === '' ? 0 : Number(input.min);
  const max = input.max === '' ? 100 : Number(input.max);
  const step = input.step === '' || input.step === 'any'
    ? 1 : Math.abs(Number(input.step)) || 1;
  return { min, max, step };
}

/**
 * The value one wheel notch away.
 *
 * `dir` is -1 for a scroll up, which increases: up means more, the way it
 * does everywhere else.
 *
 * Moves to the next point on the step grid *in that direction*, rather than
 * adding a step and rounding. The difference only shows on a value that is
 * already off-grid, and there the rounding version is lopsided: from 7 with a
 * step of 5 it goes up to 10 but down to 0, because 7 - 5 = 2 rounds to 0.
 * Stepping by grid index gives 10 and 5, which is what one notch means.
 *
 * The grid is measured from `min`, so a range like -100..100 by 3 lands on
 * the points the slider can actually show.
 */
export function nextValue(value, { min, max, step }, dir, coarse = false) {
  const n = coarse ? 10 : 1;
  const k = (value - min) / step;
  /* The epsilon keeps a value that is on-grid in exact arithmetic but a hair
   * off in binary - 0.3 / 0.1 is 3.0000000000000004 - from costing a notch. */
  const eps = 1e-9;
  const idx = dir < 0 ? Math.floor(k + eps) + n : Math.ceil(k - eps) - n;

  const clamped = Math.min(Math.max(min + idx * step, min), max);
  /* Steps are often fractional; binary error would otherwise show up as
   * 0.30000000000000004 in a label. */
  return Number(clamped.toFixed(6));
}

/**
 * Make the wheel adjust `input`, calling `onChange` when the value moves.
 *
 * Returns a teardown function.
 *
 * The default is prevented only when the value actually changed, so a scroll
 * that has run into either end of the range gives the page back rather than
 * swallowing it - which is what makes a slider inside a long column safe to
 * scroll past.
 */
export function attachWheel(input, onChange) {
  const onWheel = (e) => {
    if (e.deltaY === 0) return;
    const b = bounds(input);
    const before = Number(input.value);
    const after = nextValue(before, b, e.deltaY, e.shiftKey);
    if (after === before) return;              /* at an end - let the page go */

    e.preventDefault();
    input.value = String(after);
    /* The app listens for 'input', which a programmatic assignment does not
     * fire, so the preview would not follow without this. */
    input.dispatchEvent(new Event('input', { bubbles: true }));
    if (onChange) onChange(after);
  };

  /* Not passive: preventDefault is the whole point, and Chrome treats wheel
   * listeners as passive by default. */
  input.addEventListener('wheel', onWheel, { passive: false });
  return () => input.removeEventListener('wheel', onWheel);
}
