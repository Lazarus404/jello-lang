# Dates and times

`std.sys.Date` reads the clock, and builds, compares, shifts, formats and parses dates. A `Date`
is an instant seen in a time zone: its `time`, the milliseconds since 1970-01-01T00:00:00Z, and its
`offset`, the zone's minutes east of UTC. Its fields, such as `year()` and `hour()`, are the
calendar date and clock time in that zone. The module exports the type and the functions that make
dates under one name, so a program imports it both ways:

```text
import std.sys.Date as Date;
import type { Date } from std.sys.Date;
```

```jello
// expect: Saturday 24 February 2024, 09:30 -> 2024-03-02T09:30:00Z
import std.sys.Date as Date;

let meeting = Date.utc(2024, 2, 24, 9, 30);
let next = meeting.addDays(7);
meeting.format("%A %d %B %Y, %H:%M") + " -> " + next.iso()
```

## Making a date

| Function | Gives |
| --- | --- |
| `now(): Date` | the time now, in the local zone |
| `utc(year, month, day, hour = 0, minute = 0, second = 0, millisecond = 0): Date` | that date and time in UTC |
| `local(year, month, day, hour = 0, minute = 0, second = 0, millisecond = 0): Date` | that date and time in the local zone |
| `parse(text: Bytes): Date` | the date an ISO 8601 text gives |
| `fromTime(time: I64): Date` | the date `time` milliseconds after 1970-01-01T00:00:00Z, in the local zone |

Months run from 1, January, to 12, December. `utc`, `local` and `parse` take years from 1 to 9999,
and throw for a date or time that does not exist, such as February 30th or 24:00, naming the field:

```jello
// expect: Date.utc: day 30 is not from 1 to 29
import std.sys.Date as Date;

try { Date.utc(2024, 2, 30).iso() } catch (e) { Std.string(e) }
```

`parse` reads a date, `2024-02-24`, or a date and a time, `2024-02-24T09:30`, with the seconds
and a fraction of them if wanted, `09:30:15.250`, then `Z` or an offset such as `+01:00`, `-0500`
or `+05`. The `T` may be a space. A text with an offset keeps it; one without is a local time, at
midnight when it has no time. Anything else throws.

```jello
// expect: 2024-02-24T09:30:15.250+01:00 2024-02-24T08:30:15.250Z
import std.sys.Date as Date;

let d = Date.parse("2024-02-24T09:30:15.25+01:00");
d.iso() + " " + d.toUtc().iso()
```

## The local zone

A local date follows the system's time zone (on Unix, `TZ` or else `/etc/localtime`): its offset is
the one in force at its instant, so dates in summer and winter can have different offsets. When the
clocks go forward, a local time that they skip moves on by the change, so 02:30 on the night that
02:00 becomes 03:00 is 03:30; when they go back, a local time that happens twice is the earlier.
Any other date keeps the offset it was made with.

```jello
// expect: true true
import std.sys.Date as Date;

let lunch = Date.local(2024, 2, 24, 12);
let sameInstant = lunch.toUtc().time == Date.utc(2024, 2, 24, 12).time - (lunch.offset as I64) * 60000;
Std.string(lunch.hour() == 12) + " " + Std.string(sameInstant)
```

## Fields

| Method | Gives |
| --- | --- |
| `year(): I32` | the year |
| `month(): I32` | the month, 1 to 12 |
| `day(): I32` | the day of the month, 1 to 31 |
| `hour(): I32`, `minute(): I32`, `second(): I32` | the clock time |
| `millisecond(): I32` | 0 to 999 |
| `weekday(): I32` | 1, Monday, to 7, Sunday |
| `dayOfYear(): I32` | 1 to 366 |

The fields `time: I64`, `offset: I32` and `local: Bool` can be read directly. A date is a value:
its methods give new dates and leave it as it was.

## Changing and comparing

| Method | Gives |
| --- | --- |
| `addMilliseconds(n: I64)`, `addSeconds(n)`, `addMinutes(n)`, `addHours(n)` | the date that much later, or earlier for a negative `n` |
| `addDays(n)` | the same clock time `n` days later |
| `addMonths(n)`, `addYears(n)` | the same day and clock time, or the last day of a shorter month |
| `startOfDay()` | midnight at the start of its day |
| `toUtc()`, `toLocal()`, `withOffset(minutes)` | the same instant in another zone |
| `isBefore(other)`, `isAfter(other)`, `isSame(other)` | how the two instants compare, whatever their zones |
| `since(other): I64` | the milliseconds from `other` to it |

In a local date, a day is not always 24 hours: across a change of the clocks, `addDays(1)` keeps
the clock time and `addHours(24)` does not.

```jello
// expect: 2024-02-29 2024-03-24 2025-02-28 5400000 true
import std.sys.Date as Date;

let saturday = Date.utc(2024, 2, 24);
let leap = saturday.addDays(5);
let wait = Date.utc(2024, 2, 24, 10, 30).since(Date.utc(2024, 2, 24, 9));
leap.format("%F") + " " + saturday.addMonths(1).format("%F") + " " + leap.addYears(1).format("%F")
  + " " + Std.string(wait) + " " + Std.string(saturday.isBefore(leap))
```

## Formatting

`iso()` gives the date in ISO 8601, `2024-02-24T09:30:00+01:00`, with milliseconds when they are
not 0 and `Z` for UTC; `parse` reads it back in the years 1 to 9999. `format(pattern)` replaces
these codes in `pattern`, with English names whatever the system's language; any other `%` throws.

| Code | Gives | Code | Gives |
| --- | --- | --- | --- |
| `%Y` | 2024 | `%H` | 21, the hour 00 to 23 |
| `%y` | 24 | `%I` | 09, the hour 01 to 12 |
| `%m` | 02 | `%p` | PM |
| `%B` | February | `%M` | 05 |
| `%b` | Feb | `%S` | 07 |
| `%d` | 24, or 02 on the 2nd | `%L` | 042, the milliseconds |
| `%e` | 24, or ` 2`, padded with a space | `%z` | +0100 |
| `%j` | 055, the day of the year | `%Z` | +01:00, or Z for UTC |
| `%A` | Saturday | `%F` | %Y-%m-%d |
| `%a` | Sat | `%T` | %H:%M:%S |
| `%u` | 6, the weekday | `%%` | % |

```jello
// expect: Sat 24 Feb 2024, 09:05:07 PM (+0000) day 055
import std.sys.Date as Date;

let d = Date.utc(2024, 2, 24, 21, 5, 7);
d.format("%a %d %b %Y, %I:%M:%S %p (%z) day %j")
```
