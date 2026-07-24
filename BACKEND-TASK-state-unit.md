# Backend task — add the `state` canonical unit (discrete boolean sensors)

**For:** an agent/developer working in the **`bbh-app25.03`** repo (the IoT backend +
Mosquitto dynamic-security broker). You are NOT in that repo now — this file is the
handoff spec written from the firmware/contract side.

**Goal (one line):** teach the backend to accept and store a new canonical unit
**`state`** — a discrete boolean sample with `value` exactly `0` or `1` — so that
digital state-monitoring devices (starting with **Project5 UV Monitor**, 4
contactor open/closed inputs) can onboard and report.

**Authoritative spec:** `IoT_Projects/CONTRACT.md` → *Telemetry payload → Canonical
units → `state`*. When anything here disagrees with CONTRACT.md, CONTRACT.md wins.

---

## Scope: backend only. No broker change. Here's why.

- **Mosquitto / dynsec: NOTHING to change.** The broker only enforces **topic
  ACLs** (which topics a device may publish/subscribe) and never inspects payloads,
  units, or values. `state` readings publish on the **existing**
  `{topic_base}/telemetry` topic that the per-device ACL already grants. No new
  topic, no new ACL, no dynsec edit.
- **Backend: YES.** The backend is where units are validated (non-canonical units
  are quarantined as `invalid_unit`), where readings are stored, and where
  alerting/display live. It must learn that `state` is a legal unit and that its
  value domain is `{0, 1}`.

If you find yourself editing `iotMqttProvisioner.ts`'s ACL/role generation or any
Mosquitto config for this task, stop — that's out of scope.

---

## Changes

Work in this order. File names are from the firmware-side notes; **confirm exact
locations by grepping** — start with the existing unit list.

### 1. Add `state` to the canonical-unit whitelist (required)

Find where the current units are enforced:

```bash
grep -rniE "mS/cm|invalid_unit|canonical" backend/src
```

That list is currently `C`, `%`, `pH`, `mS/cm` (see `iot*.ts`, likely
`iotMqttBootstrap.ts` for claim-time filtering and/or a shared constant used by
telemetry ingest in `controllers/iot.ts`). **Add `state`.** Make sure the same
constant/whitelist is used by BOTH:
- **claim-time** sensor filtering (so a `state` sensor isn't dropped and the claim
  isn't rejected as "zero valid sensors"), and
- **telemetry ingest** unit validation (so `state` readings aren't quarantined as
  `invalid_unit`).

### 2. Constrain `state` value to `{0, 1}` (required)

CONTRACT.md requires `value ∈ {0, 1}` for unit `state`. On telemetry ingest, where
`value` is validated as a finite number, add: if `unit === 'state'`, reject/round
anything that isn't `0` or `1` (recommend: reject as invalid rather than silently
coerce, and log). `state` sensors have **no thresholds** — ensure any
numeric-threshold/alert-range logic treats a NULL threshold as "no min/max," which
it already does for other sensors.

### 3. Database: confirm `unit` accepts `state` (migration only if constrained)

The claim-approval path inserts sensors into `iot_sensors`
(`ON CONFLICT (device_id, sensor_key) DO UPDATE`), and `unit` is expected to be
free-ish text. **Check for a CHECK constraint or enum** on `iot_sensors.unit` (and
on the readings table's `unit`, if any):

```bash
grep -rniE "unit" sql-pg | grep -iE "check|enum|constraint"
```

- If `unit` is `text`/`varchar` with no constraint → **no migration needed**.
- If there's a CHECK/enum limiting units → add a **new forward migration** (do not
  edit `047_iot_schema.sql` in place) that adds `state` to the allowed set.

### 4. Display + alerting (recommended, not blocking onboarding)

- Render a `state` reading as **Open/Closed** (or energized/de-energized) rather
  than a number, keyed off `sensor_type` (e.g. `contactor`). `1 = closed/energized`,
  `0 = open`.
- Treat `state` history as a **step series** in charts (sample-and-hold), not a
  smoothed line.
- Alerting: support "alert on transition" and/or "alert when a contactor expected
  closed reads 0." Devices report the held/debounced state at their telemetry
  interval, so a transition shows up as a value change between consecutive readings.

---

## Verification / acceptance

1. **Claim accepted:** a bootstrap claim whose only sensors are `state` (e.g.
   `{"sensor_key":"contactor_1","sensor_type":"contactor","unit":"state"}` ×4) is
   accepted — sensors register in `iot_sensors`, claim is NOT rejected for "no valid
   sensors."
2. **Approved config round-trips:** after manual approval, the `device_config`
   reply lists all 4 `state` sensors with `unit:"state"`.
3. **Telemetry stored:** a reading `{"sensor_key":"contactor_1","value":1,"unit":"state"}`
   on `{topic_base}/telemetry` is stored (not quarantined), and `value:0` too.
4. **Bad value rejected:** `value:2` (or `0.5`) with `unit:"state"` is rejected as
   invalid.
5. **No broker change:** you did not touch Mosquitto/dynsec config or ACLs.

## Coordinate back

Once merged, tell the firmware side so Project5 can be flashed and onboarded
(`Project5-UV-Monitor`). Until this lands, the device will claim but the backend
will reject it for having no valid sensor — that failure mode looks identical to
"backend not responding," so don't chase it on the firmware side.
