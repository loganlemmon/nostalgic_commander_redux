'use strict';

// Host tests for the phone-side weather contract (src/pkjs/weather.js) —
// pure node, no Pebble runtime needed.

const {test} = require('node:test');
const assert = require('node:assert/strict');
const weather = require('../../src/pkjs/weather.js');

const NOW = Date.UTC(2026, 7, 8, 12, 30);

// parseForecast's third parameter is the forecast-window width in hours; the
// tests here pass it explicitly wherever the width itself is the point, and
// otherwise rely on the 12h default.

// Consecutive API-style hour stamps; ISO strings positionally match the
// "YYYY-MM-DDTHH:MM" shape the parser slices.
function isoHours(startMs, count) {
  const out = [];
  for (let i = 0; i < count; i++) out.push(new Date(startMs + i * 3600000).toISOString());
  return out;
}

// A full, realistic two-day response; individual tests wiggle one field.
function fullResponse() {
  const times = isoHours(Date.UTC(2026, 7, 8, 0, 0), 48);
  const temps = times.map(() => 15);
  temps[5] = 8;    // today low at 05:00
  temps[15] = 27;  // today high at 15:00
  temps[27] = 4;   // tomorrow low at 03:00
  temps[45] = 22;  // tomorrow high at 21:00
  const uv = times.map(() => 0);
  uv[5] = 9;                          // 05:00 — outside the coming window (before now-1h), ignored
  uv[15] = 6.3;                       // 15:00 — in window, becomes the max
  const pcp = times.map(() => null);  // API nulls dry hours
  pcp[13] = 42.4;                     // in window
  return {
    current: {
      temperature_2m: 22.6,
      weather_code: 61,
      relative_humidity_2m: 54.4,
      precipitation: 0.35,
      wind_direction_10m: 269.6,
      wind_speed_10m: 12.4,
    },
    hourly: {
      time: times,
      temperature_2m: temps,
      uv_index: uv,
      precipitation_probability: pcp,
    },
    daily: {temperature_2m_max: [28.4, 26.4], temperature_2m_min: [11.6, 9.6]},
  };
}

test('field table declares every key once, with the contract sentinel', () => {
  const fields = weather.WEATHER_FIELDS;
  assert.equal(fields.length, 18);
  assert.equal(new Set(fields.map(f => f.key)).size, 18);
  const sentinel = Object.fromEntries(fields.map(f => [f.key, f.sentinel]));
  for (const k
           of ['WEATHER_TEMP', 'WEATHER_HIGH', 'WEATHER_LOW', 'WEATHER_LOW_TOMORROW',
               'WEATHER_TEMP_HIGH_TOMORROW'])
    assert.equal(sentinel[k], -999, k);
  for (const k
           of ['WEATHER_AQI', 'WEATHER_UV', 'WEATHER_UV_NOW', 'WEATHER_HUMIDITY', 'WEATHER_PCP',
               'WEATHER_COND', 'WEATHER_PRECIP_NOW', 'WEATHER_WIND_DIRECTION', 'WEATHER_WIND_SPEED',
               'WEATHER_HI_HOUR_TODAY', 'WEATHER_LO_HOUR_TODAY', 'WEATHER_HI_HOUR_TOMORROW',
               'WEATHER_LO_HOUR_TOMORROW'])
    assert.equal(sentinel[k], -1, k);
  // The sentinel payload is complete by construction.
  assert.ok(weather.isCompleteWeatherPayload(weather.sentinelPayload()));
});

test('parseForecast maps and rounds the current block', () => {
  const out = weather.parseForecast(fullResponse(), NOW);
  assert.equal(out.WEATHER_TEMP, 23);
  assert.equal(out.WEATHER_COND, 61);  // the raw code; the watch maps the word
  assert.equal(out.WEATHER_HUMIDITY, 54);
  assert.equal(out.WEATHER_WIND_DIRECTION, 270);
  assert.equal(out.WEATHER_WIND_SPEED, 12);
  assert.equal(out.WEATHER_PRECIP_NOW, 4);  // 0.35mm → tenths
  assert.ok(weather.isCompleteWeatherPayload(out));
});

test('parseForecast falls back to sentinels per field', () => {
  const out = weather.parseForecast({}, NOW);
  assert.equal(out.WEATHER_TEMP, -999);
  // A missing weather code is the sentinel now — the face reads '--'.
  assert.equal(out.WEATHER_COND, -1);
  assert.equal(out.WEATHER_HUMIDITY, -1);
  assert.equal(out.WEATHER_UV, -1);
  assert.equal(out.WEATHER_HIGH, -999);
  assert.equal(out.WEATHER_HI_HOUR_TODAY, -1);
  assert.ok(weather.isCompleteWeatherPayload(out));
});

test('UV and PCP are maxima over the coming window, including the in-progress hour', () => {
  const json = fullResponse();
  json.hourly.uv_index[12] = 8;  // 12:00 — the partly-elapsed hour counts
  const out = weather.parseForecast(json, NOW);
  assert.equal(out.WEATHER_UV, 8);    // 6.3 at 15:00 also in-window, but 8 wins
  assert.equal(out.WEATHER_PCP, 42);  // nulls ignored, 42.4 rounded

  // Explicit 12 must behave exactly like the default.
  assert.equal(weather.parseForecast(json, NOW, 12).WEATHER_UV, 8);
  // And the out-of-window spike at 05:00 was indeed excluded: without idx 12,
  // 6.3 stands, not 9.
  delete json.hourly.uv_index[12];
  assert.equal(weather.parseForecast(json, NOW).WEATHER_UV, 6);
});

test('a "Now" window reads the in-progress hour only', () => {
  const json = fullResponse();
  json.hourly.uv_index[12] = 3;
  const out = weather.parseForecast(json, NOW, 0);
  assert.equal(out.WEATHER_UV, 3);    // the 12:00 bucket alone; 15:00's 6.3 excluded
  assert.equal(out.WEATHER_PCP, -1);  // the 42.4 sits at 13:00 — just out of reach
});

test('short windows bound the maxima tightly', () => {
  const out2 = weather.parseForecast(fullResponse(), NOW, 2);
  assert.equal(out2.WEATHER_PCP, 42);  // 13:00 inside a 2h window
  assert.equal(out2.WEATHER_UV, 0);    // the in-window hours read UV 0; 15:00's 6.3 stays out
  const out24 = weather.parseForecast(fullResponse(), NOW, 24);
  assert.equal(out24.WEATHER_UV, 6);  // 15:00 well inside a 24h window
});

// The bug this split exists for: at 23:30 a 12h window reaches into tomorrow
// morning, so the peak is a daylight number while the sun is down. The peak
// keeps saying so; the plain UV reading must not.
test('the instant reading is the live hour, not the window peak', () => {
  const json = fullResponse();
  const NIGHT = Date.UTC(2026, 7, 8, 23, 30);
  json.hourly.uv_index[23] = 0;    // 23:00 tonight — the in-progress hour
  json.hourly.uv_index[34] = 5.4;  // 10:00 tomorrow — inside the coming window

  const out = weather.parseForecast(json, NIGHT);
  assert.equal(out.WEATHER_UV, 5);      // the peak still reaches ahead
  assert.equal(out.WEATHER_UV_NOW, 0);  // the sun is down, and the face says so
});

test('the instant reading is absent, not zero, when the hour has no bucket', () => {
  const json = fullResponse();
  // A response whose hourly series stops before the current hour: no bucket
  // to read, which is "no data" — never a reassuring 0.
  json.hourly.uv_index = json.hourly.uv_index.map(() => null);
  const out = weather.parseForecast(json, NOW);
  assert.equal(out.WEATHER_UV_NOW, -1);
  assert.equal(weather.parseForecast({}, NOW).WEATHER_UV_NOW, -1);
});

test('a window of N hours spans exactly N buckets, the in-progress one first', () => {
  const json = fullResponse();
  const NIGHT = Date.UTC(2026, 7, 8, 23, 30);
  json.hourly.uv_index[34] = 5.4;  // 10:00 tomorrow — the 12th bucket, in
  json.hourly.uv_index[35] = 9;    // 11:00 tomorrow — the 13th, out
  assert.equal(weather.parseForecast(json, NIGHT, 12).WEATHER_UV, 5);
  // Widen by one and the spike lands.
  assert.equal(weather.parseForecast(json, NIGHT, 13).WEATHER_UV, 9);
});

test('bucket stamps are read through the response offset, not the phone clock', () => {
  const json = fullResponse();
  json.utc_offset_seconds = -25200;  // the forecast location is on PDT
  json.hourly.uv_index[12] = 4;      // 12:00 *there* = 19:00 UTC

  // 19:30 UTC is 12:30 at the forecast location: the 12:00 bucket is live.
  assert.equal(weather.parseForecast(json, Date.UTC(2026, 7, 8, 19, 30)).WEATHER_UV_NOW, 4);
  // Without the offset the same instant would read the 19:00 bucket instead,
  // which is what a bare Date() parse does to a travelling phone.
  delete json.utc_offset_seconds;
  assert.equal(weather.parseForecast(json, Date.UTC(2026, 7, 8, 19, 30)).WEATHER_UV_NOW, 0);
});

test('half-hour timezones land on the right bucket', () => {
  const json = fullResponse();
  json.utc_offset_seconds = 19800;  // +05:30
  json.hourly.uv_index[12] = 7;
  // 06:45 UTC is 12:15 there — inside the 12:00 bucket, which starts at
  // 06:30 UTC. Flooring against UTC hours instead would pick 11:00.
  assert.equal(weather.parseForecast(json, Date.UTC(2026, 7, 8, 6, 45)).WEATHER_UV_NOW, 7);
});

test('windowHoursFromClaySettings: 12h default in every no-settings shape', () => {
  assert.equal(weather.windowHoursFromClaySettings(undefined), 12);
  assert.equal(weather.windowHoursFromClaySettings({}), 12);  // no Clay save yet
  assert.equal(weather.windowHoursFromClaySettings({SETTINGS_WEATHER_WINDOW: 'bogus'}), 12);
});

test('windowHoursFromClaySettings: the select values read through, string or numeric', () => {
  assert.equal(weather.windowHoursFromClaySettings({SETTINGS_WEATHER_WINDOW: '2'}), 2);
  assert.equal(weather.windowHoursFromClaySettings({SETTINGS_WEATHER_WINDOW: 24}), 24);
  assert.equal(weather.windowHoursFromClaySettings({SETTINGS_WEATHER_WINDOW: '0'}), 0);  // Now
});

test('extremes sink together when any one is missing', () => {
  const json = fullResponse();
  json.daily.temperature_2m_max = [28.4];  // tomorrow's max absent
  const out = weather.parseForecast(json, NOW);
  for (const k
           of ['WEATHER_HIGH', 'WEATHER_LOW', 'WEATHER_LOW_TOMORROW', 'WEATHER_TEMP_HIGH_TOMORROW'])
    assert.equal(out[k], -999, k);
  // The rest of the payload still parses.
  assert.equal(out.WEATHER_TEMP, 23);
});

test('extreme event hours are per-day argmin/argmax', () => {
  const out = weather.parseForecast(fullResponse(), NOW);
  assert.equal(out.WEATHER_LO_HOUR_TODAY, 5);
  assert.equal(out.WEATHER_HI_HOUR_TODAY, 15);
  assert.equal(out.WEATHER_LO_HOUR_TOMORROW, 3);
  assert.equal(out.WEATHER_HI_HOUR_TOMORROW, 21);
});

test('a single-day series leaves tomorrow hours unknown', () => {
  const json = fullResponse();
  json.hourly.time = json.hourly.time.slice(0, 24);
  json.hourly.temperature_2m = json.hourly.temperature_2m.slice(0, 24);
  const out = weather.parseForecast(json, NOW);
  assert.equal(out.WEATHER_LO_HOUR_TODAY, 5);
  assert.equal(out.WEATHER_HI_HOUR_TODAY, 15);
  assert.equal(out.WEATHER_LO_HOUR_TOMORROW, -1);
  assert.equal(out.WEATHER_HI_HOUR_TOMORROW, -1);
});

test('the WMO code crosses the wire untouched, rounded when fractional', () => {
  const json = fullResponse();
  json.current.weather_code = 61.6;
  assert.equal(weather.parseForecast(json, NOW).WEATHER_COND, 62);
  json.current.weather_code = 96;
  assert.equal(weather.parseForecast(json, NOW).WEATHER_COND, 96);

  // Missing, null, or non-finite codes are the sentinel, not an invented word
  for (const junk of [undefined, null, NaN, '3']) {
    json.current.weather_code = junk;
    assert.equal(weather.parseForecast(json, NOW).WEATHER_COND, -1, `${junk}`);
  }
});

test('parseAqi rounds real values and reads junk as no-data', () => {
  assert.equal(weather.parseAqi({current: {us_aqi: 42.6}}), 43);
  assert.equal(weather.parseAqi({current: {us_aqi: null}}), -1);  // null is not clean air
  assert.equal(weather.parseAqi({}), -1);
});

test('a cache from an older build fails completeness', () => {
  const full = weather.parseForecast(fullResponse(), NOW);
  assert.ok(weather.isCompleteWeatherPayload(full));
  delete full.WEATHER_UV;
  assert.ok(!weather.isCompleteWeatherPayload(full));
});

test('cache freshness: inside the window fresh, at the edge already stale', () => {
  assert.equal(weather.isFreshWeatherCache(NOW - 1000, NOW), true);
  assert.equal(weather.isFreshWeatherCache(NOW - weather.WEATHER_CACHE_MAX_AGE_MS, NOW), false);
  assert.equal(weather.isFreshWeatherCache(NOW - weather.WEATHER_CACHE_MAX_AGE_MS - 1, NOW), false);
});

test('unitsFromClaySettings: imperial is the operative default, in every no-settings shape', () => {
  const imperial = {tempUnit: 'fahrenheit', windSpeedUnit: 'mph'};
  assert.deepEqual(weather.unitsFromClaySettings(undefined), imperial);
  assert.deepEqual(weather.unitsFromClaySettings({}), imperial);  // no Clay save yet
  assert.deepEqual(weather.unitsFromClaySettings({SETTINGS_UNITS: '0'}), imperial);
  assert.deepEqual(weather.unitsFromClaySettings({SETTINGS_UNITS: 'bogus'}), imperial);
});

test('unitsFromClaySettings: metric exactly on the select\'s 1, string or numeric', () => {
  const metric = {tempUnit: 'celsius', windSpeedUnit: 'ms'};
  assert.deepEqual(weather.unitsFromClaySettings({SETTINGS_UNITS: '1'}), metric);
  assert.deepEqual(weather.unitsFromClaySettings({SETTINGS_UNITS: 1}), metric);
});

test('cache freshness: garbage and future timestamps are never fresh', () => {
  assert.equal(weather.isFreshWeatherCache(NaN, NOW), false);
  assert.equal(weather.isFreshWeatherCache(undefined, NOW), false);
  assert.equal(weather.isFreshWeatherCache(NOW + 1000, NOW), false);
  assert.equal(weather.isFreshWeatherCache(NOW - 1000, NaN), false);
});
