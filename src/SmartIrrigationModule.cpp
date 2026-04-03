#include "ModuleVersionCheck.h"
#include "SmartIrrigationModule.h"
#include "versions.h"

#include <algorithm>
#include <cmath>
#include <ctime>

#include "OpenKNX/DateTime.h"

namespace
{
    // Helper function to calculate day of year (1-366)
    inline uint16_t calculateDayOfYear(uint8_t month, uint8_t day)
    {
        static const uint16_t daysBeforeMonth[] = {0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        if (month < 1 || month > 12) return 1;
        return daysBeforeMonth[month] + day;
    }

    constexpr uint16_t kKoSingleOffset = 995;
    constexpr uint8_t kKoSingleCount = 55;
    constexpr uint16_t kKoZoneBase = 1100;
    constexpr uint8_t kKoZoneBlockSize = 17; // 16 standard + 1 per-zone soil moisture
    constexpr uint32_t kSensorTimeoutWindowMs = 300000;
    constexpr uint8_t kSensorTimeoutMaxWindows = 3;
    // Phase 1 constants
    constexpr uint8_t kCheckBackMaxRetries = 3;
    constexpr uint8_t kErrorCodeValveMismatch = 20;
    constexpr uint8_t kErrorCodeValveStuck = 23;
    constexpr uint8_t kErrorCodeFlowLow = 21;
    constexpr uint8_t kErrorCodeFlowHigh = 22;
    constexpr uint32_t kFlowAlarmThresholdMs = 60000;   // 60 s before flow alarm fires
    constexpr uint32_t kFlowSensorTimeoutMs = 300000;   // 5 min → sensor invalid
    constexpr uint32_t kFlowSettleDelayMs = 10000;      // 10 s after preamble before checking

    // Priority calculation weights
    constexpr float kPriorityFixedWindowBonus = 500.0f;
    constexpr float kPriorityManualBonus = 20000.0f;
    constexpr float kPrioritySoilOverrideBonus = 10000.0f;
    constexpr float kPriorityMaxDemandPoints = 100.0f;
    constexpr float kPriorityMaxTimePoints = 100.0f;
    constexpr float kPriorityMaxUrgencyPoints = 50.0f;
    constexpr float kPriorityMaxCyclesPoints = 50.0f;
    constexpr float kPriorityWeekOveruseThreshold = 0.8f;
    constexpr float kPriorityWeekOverusePenalty = 100.0f;
    constexpr float kHoursPerWeek = 168.0f;

    enum SingleKoNumber : uint16_t
    {
        kKoSystemOnOff = 1,
        kKoSystemStatus = 2,
        kKoSystemError = 3,
        kKoEmergencyStop = 4,
        kKoManualMode = 5,
        kKoManualModeStatus = 6,
        kKoActiveZones = 7,
        kKoWeatherUpdateTrigger = 8,
        kKoWeatherUpdateStatus = 9,
        kKoWeatherUpdateLast = 10,
        kKoRainLockActive = 11,
        kKoRainLockRemaining = 12,
        kKoFlowSensor = 13,
        kKoSensorTemperature = 20,   // Temperatursensor (für Hitzebonus)
        kKoSensorRain = 21,           // Regensensor bool (Regen-Sperrzeit)
        kKoSensorRainAmount = 22,     // Regenmenge (Sperrzeitdauer)
        kKoSensorWind = 24,           // Windgeschwindigkeit (Wind-Pause)
        kKoSensorStatus = 27,         // Sensor Status (Ausgang)
        kKoForecastET0Today = 54,   // ET0 Tag 0
        kKoForecastET0Tomorrow = 38, // ET0 Tag 1
        // Weekly planning: ET0 Tag 2-6 + Rain Tag 0-6
        kKoForecastET0Day2 = 407,
        kKoForecastET0Day3 = 408,
        kKoForecastET0Day4 = 409,
        kKoForecastET0Day5 = 410,
        kKoForecastET0Day6 = 411,
        kKoForecastRainDay0 = 412,
        kKoForecastRainDay1 = 413,
        kKoForecastRainDay2 = 414,
        kKoForecastRainDay3 = 415,
        kKoForecastRainDay4 = 416,
        kKoForecastRainDay5 = 417,
        kKoForecastRainDay6 = 418,
        kKoMasterValve = 18,
        kKoAdjustmentFactor = 19,
        kKoFlowAlarm = 32,
        // Phase 3: Tank (3.1)
        kKoTankLevel = 14,
        kKoTankAlarm = 36,
        kKoWaterSourceSwitch = 29,
        // Phase 3: Wind Pause (3.5)
        kKoWindPauseActive = 35,
        // Phase 4: Suspend (4.2)
        kKoSuspendHours = 17,
        kKoSuspendActive = 31,
        // Erstinbetriebnahme
        kKoFirstStartRelease = 25,   // Eingang: Per-Objekt Freigabe (Modus 2)
        kKoFirstStartActive = 26,    // Ausgang: Status "Erststart ausstehend"
        kKoDiagMode = 400,
        kKoDiagEt0 = 401,
        kKoDiagEffectiveRain = 402,
        kKoDiagWaterDemand = 403,
        kKoDiagDecisionMode = 404,
        kKoDiagSequenceStatus = 405,
        kKoDiagFallback = 406
    };

    enum ZoneKoOffset : uint8_t
    {
        kZoneKoManualStart = 0,      // Manueller Start (S)
        kZoneKoState = 1,            // Zustand (Ü)
        kZoneKoValve = 2,            // Ventil (S+Ü bidirektional)
        kZoneKoRemaining = 3,        // Restlaufzeit
        kZoneKoWeekAmount = 4,       // Wochenmenge aktuell
        kZoneKoWeekAmountPrev = 5,   // Wochenmenge vorherige
        kZoneKoLastTime = 6,         // Letzte Bewässerung Zeit
        kZoneKoLastAmount = 7,       // Letzte Bewässerung Menge
        kZoneKoNextTime = 8,         // Nächste Bewässerung Zeit
        kZoneKoNextAmount = 9,       // Nächste Bewässerung Menge
        kZoneKoErrorCode = 10,       // Fehlercode (0=OK)
        kZoneKoPause = 11,           // Pause
        kZoneKoResetWeek = 12,       // Wochenzähler reset
        kZoneKoCycles = 13,          // Bewässerungszyklen Woche
        kZoneKoValveFeedback = 14,   // Ventil-Rückmeldung
        kZoneKoVerifyFailed = 15,    // Verifizierung fehlgeschlagen
        kZoneKoSoilMoisture = 16,    // Bodenfeuchte % (Eingang, per Zone)
        kZoneKoActivityInput = 17    // Aktivitätsblock-Eingang (Eingang, per Zone)
    };

    enum ZoneState : uint8_t
    {
        kZoneStateInactive = 0,
        kZoneStateWaiting = 1,
        kZoneStateActive = 2,
        kZoneStatePaused = 3,
        kZoneStateSoaking = 4
    };

    uint16_t singleKoNumber(uint16_t number)
    {
        return static_cast<uint16_t>(kKoSingleOffset + number - 1);
    }

    uint16_t zoneKoNumber(uint8_t zoneIndex, uint8_t offset)
    {
        return static_cast<uint16_t>(kKoZoneBase + zoneIndex * kKoZoneBlockSize + offset);
    }

    bool isValidRange(float value, float minValue, float maxValue)
    {
        if (std::isnan(value) || std::isinf(value))
        {
            return false;
        }
        return value >= minValue && value <= maxValue;
    }

    bool shouldTreatAsValid(bool enabled, bool hasValue, bool failed)
    {
        return enabled && hasValue && !failed;
    }

    float round2(float value)
    {
        return std::round(value * 100.0f) / 100.0f;
    }

    uint16_t minutesToHoursCeil(uint16_t minutes)
    {
        if (minutes == 0)
        {
            return 0;
        }
        return static_cast<uint16_t>((minutes + 59) / 60);
    }

    // M-4: clampFloat moved to SmartIrrigationCore.h
    using SmartIrrigation::clampFloat;
    // N-1/N-2: isWindowActive() and isFixedWindowMatch() defined in SmartIrrigationCore.h
    using SmartIrrigation::isWindowActive;
    using SmartIrrigation::isFixedWindowMatch;

    uint16_t minutesUntilEnd(uint16_t nowMinutes, uint16_t endMinutes)
    {
        if (endMinutes >= nowMinutes)
        {
            return static_cast<uint16_t>(endMinutes - nowMinutes);
        }
        return static_cast<uint16_t>((1440 - nowMinutes) + endMinutes);
    }

    void writeDateTimeKo(uint16_t koNumber, uint32_t epochSec, bool timeValid)
    {
        if (!timeValid)
        {
            return;
        }

        OpenKNX::DateTime dt(static_cast<time_t>(epochSec));
        tm knxTime = dt.toTm();
        knx.getGroupObject(koNumber).value(knxTime, DPT_DateTime);
    }

    // Phase 2.1: Resolve time window for Sunrise/Sunset based modes
    // Modifies settings.window1 in-place based on sun position
    void resolveTimeWindowForSun(SmartIrrigation::ZoneSettings &settings)
    {
        using SmartIrrigation::TimeWindowType;

        if (settings.windowType != TimeWindowType::SunriseBased &&
            settings.windowType != TimeWindowType::SunsetBased)
        {
            return; // Nothing to resolve for Flexible/Fixed
        }

#ifdef ParamBASE_Latitude
        // Check if sun calculation is valid (not polar night/midnight sun)
        if (!openknx.sun.isSunCalculatioValid())
        {
            // Fallback: use static window1 settings as-is, treat as Flexible
            settings.windowType = TimeWindowType::Flexible;
            return;
        }

        // Check if coordinates are configured (lat=0 AND lon=0 means not configured)
        const float lat = ParamBASE_Latitude;
        const float lon = ParamBASE_Longitude;
        if (std::abs(lat) < 0.0001f && std::abs(lon) < 0.0001f)
        {
            settings.windowType = TimeWindowType::Flexible;
            return;
        }

        uint16_t sunMinutes = 0;
        if (settings.windowType == TimeWindowType::SunriseBased)
        {
            const auto t = openknx.sun.sunRiseLocalTime();
            sunMinutes = static_cast<uint16_t>(t.hour) * 60 + t.minute;
        }
        else // SunsetBased
        {
            const auto t = openknx.sun.sunSetLocalTime();
            sunMinutes = static_cast<uint16_t>(t.hour) * 60 + t.minute;
        }

        // Apply offset (can be negative for before sunrise/sunset)
        int32_t startMinutes = static_cast<int32_t>(sunMinutes) + settings.sunOffsetMinutes;
        // Wrap around midnight
        if (startMinutes < 0)
        {
            startMinutes += 1440;
        }
        else if (startMinutes >= 1440)
        {
            startMinutes -= 1440;
        }

        uint32_t endMinutes = static_cast<uint32_t>(startMinutes) + settings.sunWindowDurationMinutes;
        if (endMinutes >= 1440)
        {
            endMinutes -= 1440;
        }

        // Override window1 with calculated values
        settings.window1.active = true;
        settings.window1.startMinutes = static_cast<uint16_t>(startMinutes);
        settings.window1.endMinutes = static_cast<uint16_t>(endMinutes);
        // Sunrise/Sunset always behaves like Flexible (no exact time match required)
        settings.windowType = TimeWindowType::Flexible;
#else
        // ParamBASE_Latitude not defined: fallback to static window, treat as Flexible
        settings.windowType = TimeWindowType::Flexible;
#endif
    }

    float calculatePriority(const SmartIrrigation::ZoneSettings &settings,
                            float weekAmount,
                            uint8_t cycles,
                            uint32_t lastTimeSec,
                            const SmartIrrigation::DecisionResult &result,
                            uint16_t nowMinutes,
                            uint32_t nowSec,
                            bool timeValid)
    {
        float priority = 0.0f;

        if (settings.maxWeek > 0 && result.waterDemand > 0.0f)
        {
            const float demandPercent = (result.waterDemand / settings.maxWeek) * kPriorityMaxDemandPoints;
            priority += clampFloat(demandPercent, 0.0f, kPriorityMaxDemandPoints);
        }

        if (timeValid)
        {
            float hoursSince = 0.0f;
            if (lastTimeSec > 0 && nowSec >= lastTimeSec)
            {
                hoursSince = static_cast<float>(nowSec - lastTimeSec) / 3600.0f;
            }
            else if (lastTimeSec == 0)
            {
                hoursSince = kHoursPerWeek;
            }

            const float timePercent = clampFloat((hoursSince / kHoursPerWeek) * kPriorityMaxTimePoints, 0.0f, kPriorityMaxTimePoints);
            priority += timePercent;
        }

        if (settings.windowType == SmartIrrigation::TimeWindowType::Flexible)
        {
            bool inWindow = false;
            uint16_t minutesToEnd = 0;

            if (isWindowActive(nowMinutes, settings.window1))
            {
                inWindow = true;
                minutesToEnd = minutesUntilEnd(nowMinutes, settings.window1.endMinutes);
            }
            else if (isWindowActive(nowMinutes, settings.window2))
            {
                inWindow = true;
                minutesToEnd = minutesUntilEnd(nowMinutes, settings.window2.endMinutes);
            }

            if (inWindow && minutesToEnd <= 60)
            {
                priority += static_cast<float>(60 - minutesToEnd) * (kPriorityMaxUrgencyPoints / 60.0f);
            }
        }
        else
        {
            // Fixed-window zones only have priority at their exact start time;
            // outside the start window they return 0 so they won't compete for slots
            // (decideWatering already prevents starts outside fixed windows).
            const bool fixMatch = (settings.window1.active && isFixedWindowMatch(nowMinutes, settings.window1.startMinutes)) ||
                                  (settings.window2.active && isFixedWindowMatch(nowMinutes, settings.window2.startMinutes));
            if (fixMatch)
            {
                priority += kPriorityFixedWindowBonus;
                return priority;
            }
            return 0.0f;
        }

        float cyclesPoints = kPriorityMaxCyclesPoints;
        if (settings.maxCycles > 0)
        {
            const float cyclesPercent = clampFloat(static_cast<float>(cycles) / settings.maxCycles, 0.0f, 1.0f);
            cyclesPoints = (1.0f - cyclesPercent) * kPriorityMaxCyclesPoints;
        }
        priority += cyclesPoints;

        if (settings.maxWeek > 0)
        {
            const float weekPercent = weekAmount / settings.maxWeek;
            if (weekPercent > kPriorityWeekOveruseThreshold)
            {
                priority -= (weekPercent - kPriorityWeekOveruseThreshold) * kPriorityWeekOverusePenalty;
            }
        }

        // User-configured zone priority: 0-10, default 5 = neutral.
        // Each step adds/subtracts 20 points (range: -100 to +100).
        priority += (static_cast<float>(settings.zonePriority) - 5.0f) * 20.0f;

        return priority;
    }
}


const std::string SmartIrrigationModule::name()
{
    return "SmartIrrigation";
}

const std::string SmartIrrigationModule::version()
{
    return MODULE_SmartIrrigationModule_Version;
}

void SmartIrrigationModule::init()
{
    std::fill(sensorLastValidMs_.begin(), sensorLastValidMs_.end(), 0);
    weatherUpdateLastSec_ = 0;
    lastDecisionMs_ = 0;
    rainLock_.remainingHours = 0.0f;
    rainLock_.lastUpdateMs = millis();
    rainLock_.active = false;
    rainLock_.lastSentHours = 0;
    rainLock_.lastSentActive = false;
}

void SmartIrrigationModule::setup(bool configured)
{
    (void)configured;
}

void SmartIrrigationModule::loop(bool configured)
{
    (void)configured;

    const uint32_t nowMs = millis();

    // Erstinbetriebnahme: detect first commissioning and apply configured mode (runs exactly once)
    if (!firstStartChecked_)
    {
        firstStartChecked_ = true;
        bool isFirstStart = true;
        const uint8_t zoneCount = std::min<uint8_t>(ParamSIR_SIR_ZoneCount, kMaxZones);
        for (uint8_t i = 0; i < zoneCount; ++i)
        {
            if (zoneRuntime_[i].weekAmount > 0.0f || zoneRuntime_[i].cycles > 0 ||
                zoneRuntime_[i].windowStartDayOfYear > 0)
            {
                isFirstStart = false;
                break;
            }
        }
        if (isFirstStart)
        {
            const uint8_t mode = ParamSIR_SIR_FirstStartMode;
            if (mode == 1)
            {
                // Zeitverzögerung
                const uint32_t delayMs = static_cast<uint32_t>(ParamSIR_SIR_FirstStartDelay) * 3600000UL;
                if (delayMs > 0)
                {
                    firstStartBlocked_ = true;
                    firstStartReleaseAtMs_ = nowMs + delayMs;
                    knx.getGroupObject(singleKoNumber(kKoFirstStartActive)).value(true, DPT_Switch);
                    logInfoP("Erstinbetriebnahme: Zeitverzögerung %u h aktiv",
                             (unsigned)ParamSIR_SIR_FirstStartDelay);
                }
            }
            else if (mode == 2)
            {
                // Per Objekt
                firstStartBlocked_ = true;
                knx.getGroupObject(singleKoNumber(kKoFirstStartActive)).value(true, DPT_Switch);
                logInfoP("Erstinbetriebnahme: Warte auf KO-Freigabe (Per Objekt)");
            }
            // mode == 0 (Sofort): no block, first irrigation proceeds normally
        }
    }

    // Erstinbetriebnahme: release when time-delay has elapsed (mode 1)
    if (firstStartBlocked_ && firstStartReleaseAtMs_ > 0 && nowMs >= firstStartReleaseAtMs_)
    {
        firstStartBlocked_ = false;
        firstStartReleaseAtMs_ = 0;
        knx.getGroupObject(singleKoNumber(kKoFirstStartActive)).value(false, DPT_Switch);
        logInfoP("Erstinbetriebnahme: Zeitverzögerung abgelaufen, erste Bewässerung freigegeben");
    }

    if (!shouldProcessDecision(nowMs))
    {
        updateRainLockCountdown();
        return;
    }

    lastDecisionMs_ = nowMs;

    updateRainLockCountdown();

    const bool systemOn = knx.getGroupObject(singleKoNumber(kKoSystemOnOff)).value(DPT_Switch);
    const bool emergencyStop = knx.getGroupObject(singleKoNumber(kKoEmergencyStop)).value(DPT_Switch);
    const bool manualMode = knx.getGroupObject(singleKoNumber(kKoManualMode)).value(DPT_Switch);
    knx.getGroupObject(singleKoNumber(kKoManualModeStatus)).value(manualMode, DPT_Switch);
    const bool manualProgram = ParamSIR_SIR_Program == 6;
    const bool timeProgram = ParamSIR_SIR_Program == 2;
    const bool manualActive = manualMode || manualProgram;

    bool timeValid = openknx.time.isValid();
    auto localTime = openknx.time.getLocalTime();
    const uint16_t nowMinutes = timeValid ? static_cast<uint16_t>(localTime.hour * 60 + localTime.minute) : 0;
    const uint8_t month = timeValid ? localTime.month : 1;
    const uint32_t nowSec = timeValid ? static_cast<uint32_t>(localTime.toTime_t())
                                      : static_cast<uint32_t>(millis() / 1000);

    // Phase 4.2: Check Suspend timeout
    if (suspendActive_ && suspendEndTime_ > 0 && timeValid && nowSec >= suspendEndTime_)
    {
        suspendActive_ = false;
        suspendEndTime_ = 0;
        knx.getGroupObject(singleKoNumber(kKoSuspendActive)).value(false, DPT_Switch);
        logInfoP("Suspend ended (timeout reached)");
        saveRuntimeToFlash(true); // AD-6: persist cancel
    }

    // AD-6: NTP robustness - clear Suspend if NTP invalid for >10 min
    if (suspendActive_)
    {
        if (!timeValid)
        {
            if (ntpInvalidSinceMs_ == 0)
            {
                ntpInvalidSinceMs_ = nowMs;
            }
            else if (nowMs - ntpInvalidSinceMs_ > 10UL * 60UL * 1000UL)  // >10 minutes
            {
                // NTP invalid too long → cancel Suspend (conservatively: water rather than let plants die)
                suspendActive_ = false;
                suspendEndTime_ = 0;
                ntpInvalidSinceMs_ = 0;
                knx.getGroupObject(singleKoNumber(kKoSuspendActive)).value(false, DPT_Switch);
                logInfoP("Suspend cancelled (NTP invalid >10 min)");
                saveRuntimeToFlash(true); // AD-6: persist cancel
            }
        }
        else
        {
            ntpInvalidSinceMs_ = 0;  // NTP valid again, reset timer
        }
    }
    else
    {
        ntpInvalidSinceMs_ = 0;  // No suspend active, no need to track
    }

    const bool sensorOk = true; // All sensors optional; system degrades gracefully
    updateSensorStatusKo(sensorOk);

    const uint8_t zoneCount = std::min<uint8_t>(ParamSIR_SIR_ZoneCount, kMaxZones);
    uint8_t activeZones = 0;

    SmartIrrigation::DecisionResult bestDiag{};
    uint8_t bestDiagZone = 0;
    float bestDemand = -1.0f;

    const float minTempLimitC = static_cast<float>(ParamSIR_SIR_MinTemp);
    const bool hasAmbientTemperature = weatherCache_.real.hasTemperature;
    const float ambientTemperatureC = weatherCache_.real.temperatureC;
    const bool minTempStartBlocked = hasAmbientTemperature && ambientTemperatureC < minTempLimitC;

    // 1.4 Post-Freeze Delay: track when minTemp block clears
    if (minTempStartBlocked)
    {
        wasMinTempBlocked_ = true;
    }
    else if (wasMinTempBlocked_)
    {
        wasMinTempBlocked_ = false;
        freezeEndSec_ = nowSec;
    }

    const uint8_t postFreezeDelayHours = ParamSIR_SIR_PostFreezeDelay;
    const bool postFreezeBlocked = !manualActive &&
                                   postFreezeDelayHours > 0 &&
                                   freezeEndSec_ > 0 &&
                                   nowSec < freezeEndSec_ + static_cast<uint32_t>(postFreezeDelayHours) * 3600u;

    // 3.4 Season check: block automatic starts outside configured season
    const bool seasonEnabled = ParamSIR_SIR_SeasonEnabled;
    const uint8_t seasonStartMonth = ParamSIR_SIR_SeasonStartMonth;
    const uint8_t seasonEndMonth = ParamSIR_SIR_SeasonEndMonth;
    bool seasonBlocked = false;
    if (seasonEnabled && !manualActive && timeValid)
    {
        if (seasonStartMonth <= seasonEndMonth)
        {
            // Normal range: e.g. March(3) - October(10)
            seasonBlocked = (month < seasonStartMonth || month > seasonEndMonth);
        }
        else
        {
            // Wrap-around range: e.g. November(11) - February(2) (southern hemisphere)
            seasonBlocked = (month < seasonStartMonth && month > seasonEndMonth);
        }
    }

    // 3.5 Wind Pause: check system-wide wind threshold
    const uint8_t windPauseThresholdKmh = ParamSIR_SIR_WindPauseThreshold;
    const float currentWindKmh = weatherCache_.real.hasWind ? weatherCache_.real.windSpeedMs * 3.6f : 0.0f;
    const bool windExceedsThreshold = windPauseThresholdKmh > 0 && currentWindKmh > static_cast<float>(windPauseThresholdKmh);
    const bool windBelowHysteresis = windPauseThresholdKmh > 0 && currentWindKmh < static_cast<float>(windPauseThresholdKmh) * 0.8f;
    if (windExceedsThreshold && !windPauseActive_)
    {
        windPauseActive_ = true;
        knx.getGroupObject(singleKoNumber(kKoWindPauseActive)).value(true, DPT_Switch);
    }
    else if (windBelowHysteresis && windPauseActive_)
    {
        windPauseActive_ = false;
        knx.getGroupObject(singleKoNumber(kKoWindPauseActive)).value(false, DPT_Switch);
    }

    // 3.2 Activity Block: now per-zone — handled in zone loop below

    // 3.1 Tank low: check level and handle pause/stop/switch
    const bool tankEnabled = ParamSIR_SIR_TankEnabled;
    const uint8_t tankMinPercent = ParamSIR_SIR_TankMinPercent;
    const uint8_t tankAction = ParamSIR_SIR_TankAction; // 0=Pause, 1=Stop, 2=Quellenwechsel
    const bool tankLow = tankEnabled && tankLevelPercent_ < tankMinPercent;
    const bool tankRecovered = tankEnabled && tankLevelPercent_ >= (tankMinPercent + 5);
    if (tankLow && !tankAlarmActive_)
    {
        tankAlarmActive_ = true;
        knx.getGroupObject(singleKoNumber(kKoTankAlarm)).value(true, DPT_Switch);
        if (tankAction == 2)
        {
            // Quellenwechsel: activate city water
            knx.getGroupObject(singleKoNumber(kKoWaterSourceSwitch)).value(true, DPT_Switch);
        }
    }
    else if (tankRecovered && tankAlarmActive_)
    {
        tankAlarmActive_ = false;
        knx.getGroupObject(singleKoNumber(kKoTankAlarm)).value(false, DPT_Switch);
        tankLowPaused_ = false;
        if (tankAction == 2)
        {
            knx.getGroupObject(singleKoNumber(kKoWaterSourceSwitch)).value(false, DPT_Switch);
        }
    }

    if (!systemOn || emergencyStop)
    {
        stopAllZones(zoneCount, nowSec, timeValid);
        knx.getGroupObject(singleKoNumber(kKoActiveZones)).value(static_cast<uint8_t>(0), DPT_Value_1_Ucount);
        knx.getGroupObject(singleKoNumber(kKoSystemStatus)).value(false, DPT_Switch);
        knx.getGroupObject(singleKoNumber(kKoSystemError)).value(!sensorOk, DPT_Switch);
        return;
    }

    // 3.1 Tank low actions (Pause or Stop)
    if (tankLow && tankAction == 1 && !tankLowPaused_)
    {
        // Action 1 = Stop: stop all zones immediately
        stopAllZones(zoneCount, nowSec, timeValid);
        tankLowPaused_ = true; // use same flag to track we've taken action
    }

    updateActiveZoneCountdown(zoneCount, nowSec, timeValid);

    struct QueueEntry
    {
        uint8_t zoneIndex = 0;
        float priority = 0.0f;
        bool manual = false;
    };

    std::array<SmartIrrigation::DecisionResult, kMaxZones> decisions{};
    std::array<SmartIrrigation::ZoneSettings, kMaxZones> settingsCache{};
    std::array<uint16_t, kMaxZones> manualRuntime{};
    std::array<bool, kMaxZones> manualRequested{};
    std::array<bool, kMaxZones> pauseRequested{};
    std::array<QueueEntry, kMaxZones> queue{};
    uint8_t queueCount = 0;
    bool fallbackActiveAny = false;

    for (uint8_t zoneIndex = 0; zoneIndex < zoneCount; ++zoneIndex)
    {
        SmartIrrigation::ZoneSettings settings = loadZoneSettings(zoneIndex);

        const bool zoneManualStart = knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoManualStart)).value(DPT_Switch);
        const bool zonePause = knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoPause)).value(DPT_Switch);
        const bool zoneResetWeek = knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoResetWeek)).value(DPT_Switch);

        // Per-zone soil moisture (polled from KO, only when enabled)
        if (settings.soilMoistureEnabled)
        {
            const float soilValue = knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoSoilMoisture)).value(DPT_Value_Humidity);
            const bool valid = isValidRange(soilValue, 0.0f, 100.0f);
            hasZoneSoilMoisture_[zoneIndex] = valid;
            if (valid) zoneSoilMoisturePercent_[zoneIndex] = soilValue;
        }
        else
        {
            hasZoneSoilMoisture_[zoneIndex] = false;
        }
        const bool manualRequest = manualActive && zoneManualStart;
        manualRequested[zoneIndex] = manualRequest;
        pauseRequested[zoneIndex] = zonePause;

        settings.enabled = manualActive ? (settings.enabled && zoneManualStart && !zonePause)
                                        : (settings.enabled && !zonePause);
        settingsCache[zoneIndex] = settings;

        ZoneRuntime &runtimeState = zoneRuntime_[zoneIndex];

        if (zoneResetWeek)
        {
            runtimeState.weekAmountPrev = runtimeState.weekAmount;
            runtimeState.weekAmount = 0.0f;
            runtimeState.cycles = 0;
            runtimeState.windowStartDayOfYear = 0;

            knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoWeekAmountPrev)).value(runtimeState.weekAmountPrev,
                                                                                    DPT_Rain_Amount);
            knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoWeekAmount)).value(runtimeState.weekAmount,
                                                                                DPT_Rain_Amount);
            knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoCycles)).value(runtimeState.cycles,
                                                                            DPT_Value_1_Ucount);
            knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoResetWeek)).value(false, DPT_Switch);
            saveRuntimeToFlash(true);
        }

        // Soaking-Guard (K-2): verhindert dass decideWatering() für soakende Zonen aufgerufen wird
        if (runtimeState.state == kZoneStateSoaking)
        {
            if (!settings.enabled || zoneResetWeek)
            {
                // Zone deaktiviert oder Wochenzähler reset während Soak → abbrechen
                runtimeState.soakEndSec = 0;
                runtimeState.state = kZoneStateInactive;
                runtimeState.manualRun = false;
                continue;
            }
            if (manualRequest)
            {
                // Manueller Start cancellt Soak (K-6): soakEndSec=0, Inactive
                // → kein continue → fällt in manuellen Pfad unten durch
                runtimeState.soakEndSec = 0;
                runtimeState.state = kZoneStateInactive;
            }
            else if (runtimeState.soakEndSec > 0 && nowSec < runtimeState.soakEndSec)
            {
                // Soak-Timer läuft noch → Loop überspringen
                continue;
            }
            else
            {
                // Soak-Timer abgelaufen → Neu-Bewertung (fällt durch zu decideWatering)
                runtimeState.soakEndSec = 0;
                runtimeState.state = kZoneStateInactive;
            }
        }

        // K-3: Manual-Off — auch für Soaking-Zonen (Soaking bereits oben auf Inactive gesetzt)
        if ((runtimeState.state == kZoneStateActive || runtimeState.state == kZoneStateSoaking) &&
            runtimeState.manualRun && (!manualActive || !manualRequest))
        {
            if (runtimeState.state == kZoneStateSoaking)
            {
                runtimeState.soakEndSec = 0;
                runtimeState.state = kZoneStateInactive;
                runtimeState.manualRun = false;
            }
            else
            {
                stopZone(zoneIndex, settings, nowSec, timeValid);
            }
        }

        if (runtimeState.state == kZoneStateActive && zonePause)
        {
            runtimeState.pauseRemainingMinutes = runtimeState.remainingMinutes;
            runtimeState.pauseStartSec = nowSec;
            runtimeState.state = kZoneStatePaused;
            runtimeState.plannedRuntimeSec = 0;
            runtimeState.startTimeSec = 0;
            runtimeState.nextTimeSec = 0;
            knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoValve)).value(false, DPT_Switch);
        }

        // 3.1 Tank low pause (for active zones, action=Pause)
        if (tankLow && tankAction == 0 && runtimeState.state == kZoneStateActive && !runtimeState.tankPaused)
        {
            runtimeState.tankPaused = true;
            knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoValve)).value(false, DPT_Switch);
        }
        else if (runtimeState.tankPaused && !tankLow)
        {
            runtimeState.tankPaused = false;
            if (runtimeState.state == kZoneStateActive && !zonePause && !runtimeState.activityPaused && !runtimeState.windPaused)
            {
                knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoValve)).value(true, DPT_Switch);
            }
        }

        // 3.2 Activity Block per zone: read zone-specific KO (link to any door/presence GA)
        const bool zoneActivityInput = settings.activityEnabled &&
                                       knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoActivityInput)).value(DPT_Switch);
        if (runtimeState.state == kZoneStateActive && zoneActivityInput)
        {
            // AD-5: Soil override wins over activity block
            const bool currentSoilOverride = hasZoneSoilMoisture_[zoneIndex] &&
                                             settings.soilThresholdPercent > 0 &&
                                             zoneSoilMoisturePercent_[zoneIndex] < static_cast<float>(settings.soilThresholdPercent);
            if (!currentSoilOverride && !runtimeState.manualRun && !runtimeState.activityPaused)
            {
                runtimeState.activityPaused = true;
                knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoValve)).value(false, DPT_Switch);
            }
        }
        else if (runtimeState.activityPaused && !zoneActivityInput)
        {
            // Activity block ended, resume valve
            runtimeState.activityPaused = false;
            if (runtimeState.state == kZoneStateActive && !zonePause && !runtimeState.windPaused)
            {
                knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoValve)).value(true, DPT_Switch);
            }
        }

        // 3.5 Wind Pause per zone (only sprinklers, manual also affected)
        if (runtimeState.state == kZoneStateActive && settings.isSprinkler && windPauseActive_)
        {
            if (!runtimeState.windPaused)
            {
                runtimeState.windPaused = true;
                knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoValve)).value(false, DPT_Switch);
            }
        }
        else if (runtimeState.windPaused && !windPauseActive_)
        {
            // Wind pause ended, resume valve
            runtimeState.windPaused = false;
            if (runtimeState.state == kZoneStateActive && !zonePause && !runtimeState.activityPaused)
            {
                knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoValve)).value(true, DPT_Switch);
            }
        }

        if (runtimeState.state == kZoneStatePaused)
        {
            if (runtimeState.pauseStartSec > 0)
            {
                const uint32_t pauseElapsedSec = nowSec >= runtimeState.pauseStartSec
                                                     ? nowSec - runtimeState.pauseStartSec
                                                     : 0;
                const uint32_t pauseMinutes = pauseElapsedSec / 60;
                if (pauseMinutes > 60)
                {
                    runtimeState.state = kZoneStateInactive;
                    runtimeState.errorActive = true;
                    runtimeState.errorCode = 11;
                    runtimeState.remainingMinutes = 0;
                    runtimeState.plannedRuntimeSec = 0;
                    runtimeState.startTimeSec = 0;
                    runtimeState.nextTimeSec = 0;
                    runtimeState.nextAmount = 0.0f;
                    runtimeState.manualRun = false;
                    runtimeState.pauseRemainingMinutes = 0;
                    runtimeState.pauseStartSec = 0;
                }
            }

            if (runtimeState.state == kZoneStatePaused)
            {
                decisions[zoneIndex] = SmartIrrigation::DecisionResult{};
                continue;
            }
        }

        SmartIrrigation::DecisionContext context{};
        context.systemOn = systemOn;
        context.emergencyStop = emergencyStop;
        context.rainLockActive = timeProgram ? false : rainLock_.active;
        context.nowMinutes = nowMinutes;
        context.allowWhenNoWindow = true;
        context.minTempC = ParamSIR_SIR_MinTemp;
        context.hasSoilMoisture = hasZoneSoilMoisture_[zoneIndex];
        context.soilMoisturePercent = zoneSoilMoisturePercent_[zoneIndex];

        SmartIrrigation::ZoneRuntimeState runtime{};
        runtime.weekAmount = zoneRuntime_[zoneIndex].weekAmount;
        runtime.cycles = zoneRuntime_[zoneIndex].cycles;

        // Rolling interval reset: reset weekAmount+cycles when the window has expired
        if (timeValid && zoneRuntime_[zoneIndex].windowStartDayOfYear != 0)
        {
            const uint8_t interval = settings.irrigationIntervalDays > 0 ? settings.irrigationIntervalDays : 7;
            const uint16_t today = calculateDayOfYear(localTime.month, localTime.day);
            int16_t daysDiff = static_cast<int16_t>(today) - static_cast<int16_t>(zoneRuntime_[zoneIndex].windowStartDayOfYear);
            if (daysDiff < 0) daysDiff += 365;
            if (daysDiff >= static_cast<int16_t>(interval))
            {
                zoneRuntime_[zoneIndex].weekAmountPrev = zoneRuntime_[zoneIndex].weekAmount;
                zoneRuntime_[zoneIndex].weekAmount = 0.0f;
                zoneRuntime_[zoneIndex].cycles = 0;
                zoneRuntime_[zoneIndex].windowStartDayOfYear = 0;
                runtime.weekAmount = 0.0f;
                runtime.cycles = 0;
                knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoWeekAmountPrev)).value(zoneRuntime_[zoneIndex].weekAmountPrev, DPT_Rain_Amount);
                knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoWeekAmount)).value(0.0f, DPT_Rain_Amount);
                knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoCycles)).value(static_cast<uint8_t>(0), DPT_Value_1_Ucount);
                saveRuntimeToFlash(true);
            }
        }

        // Phase 2.1: Resolve sun-based time windows before decision
        resolveTimeWindowForSun(settings);

        SmartIrrigation::DecisionResult result = SmartIrrigation::decideWatering(settings,
                                                                                 runtime,
                                                                                 context,
                                                                                 weatherCache_);

        // Phase 3.3: Weekday Filter — block if today is not an allowed day
        if (result.action == SmartIrrigation::DecisionAction::Start &&
            !result.soilOverride && !runtimeState.manualRun && timeValid)
        {
            const uint8_t dayOfWeek = localTime.dayOfWeek; // 0=Monday, 6=Sunday
            if (!(settings.allowedWeekdays & (1 << dayOfWeek)))
            {
                result.action = SmartIrrigation::DecisionAction::None;
            }
        }

        // Phase 3.4: Season Block — block automatic starts outside season
        if (result.action == SmartIrrigation::DecisionAction::Start &&
            seasonBlocked && !result.soilOverride && !runtimeState.manualRun)
        {
            result.action = SmartIrrigation::DecisionAction::None;
        }

        // Phase 4.2: Suspend Block — block automatic starts during suspend
        // Exception: soil override still triggers when critically dry (Bug #8)
        if (result.action == SmartIrrigation::DecisionAction::Start &&
            suspendActive_ && !runtimeState.manualRun && !result.soilOverride)
        {
            result.action = SmartIrrigation::DecisionAction::None;
        }

        // Erstinbetriebnahme: block automatic starts until first-start is released
        // Exception: manual runs always pass through
        if (result.action == SmartIrrigation::DecisionAction::Start &&
            firstStartBlocked_ && !runtimeState.manualRun)
        {
            result.action = SmartIrrigation::DecisionAction::None;
        }

        // Phase 2.5: Apply external Adjustment Factor
        if (adjustmentFactorPercent_ != 100 && result.waterDemand > 0.0f)
        {
            result.waterDemand *= (static_cast<float>(adjustmentFactorPercent_) / 100.0f);
        }

        // AdjustmentFactor=0 blocks all starts (emergency brake from external system)
        // Exception: soil override still triggers when critically dry (Bug #14)
        if (adjustmentFactorPercent_ == 0 && result.action == SmartIrrigation::DecisionAction::Start
            && !result.soilOverride)
        {
            result.action = SmartIrrigation::DecisionAction::None;
        }

        decisions[zoneIndex] = result;
        fallbackActiveAny = fallbackActiveAny || result.fallbackActive;

        // Phase 4.3: Post-Irrigation Verify check
        if (settings.verifyEnabled &&
            runtimeState.verifyTimerMs > 0 &&
            runtimeState.state != kZoneStateActive)
        {
            const uint32_t verifyDelayMs = static_cast<uint32_t>(settings.verifyDelayMinutes) * 60000UL;
            if ((nowMs - runtimeState.verifyTimerMs) >= verifyDelayMs)
            {
                // Timer expired - check soil moisture delta
                const float currentSoil = hasZoneSoilMoisture_[zoneIndex]
                                              ? zoneSoilMoisturePercent_[zoneIndex]
                                              : 0.0f;
                const float deltaSoil = currentSoil - static_cast<float>(runtimeState.verifyStartSoil);
                const bool verifyFailed = deltaSoil < static_cast<float>(settings.verifyMinDeltaPercent);
                knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoVerifyFailed)).value(verifyFailed, DPT_Switch);
                runtimeState.verifyTimerMs = 0; // Clear the timer
            }
        }

        runtimeState.errorActive = result.errorCode != 0;
        runtimeState.errorCode = result.errorCode;

        if (!settings.enabled && runtimeState.state == kZoneStateActive)
        {
            stopZone(zoneIndex, settings, nowSec, timeValid);
        }

        if (manualActive)
        {
            if (!settings.enabled && runtimeState.state != kZoneStateActive)
            {
                runtimeState.state = kZoneStateInactive;
                runtimeState.remainingMinutes = 0;
                runtimeState.plannedRuntimeSec = 0;
                runtimeState.startTimeSec = 0;
                runtimeState.nextTimeSec = 0;
                runtimeState.nextAmount = 0.0f;
            }
            else if (manualRequested[zoneIndex] && runtimeState.state != kZoneStateActive)
            {
                if (minTempStartBlocked)
                {
                    runtimeState.state = kZoneStateInactive;
                    runtimeState.remainingMinutes = 0;
                    runtimeState.plannedRuntimeSec = 0;
                    runtimeState.startTimeSec = 0;
                    runtimeState.nextTimeSec = 0;
                    runtimeState.nextAmount = 0.0f;
                }
                else if (settings.absMaxWeek > 0 && runtimeState.weekAmount >= settings.absMaxWeek)
                {
                    runtimeState.errorActive = true;
                    runtimeState.errorCode = 6;
                    runtimeState.state = kZoneStateInactive;
                }
                else if (settings.maxCycles > 0 && runtimeState.cycles >= settings.maxCycles)
                {
                    runtimeState.errorActive = true;
                    runtimeState.errorCode = 6;
                    runtimeState.state = kZoneStateInactive;
                }
                else
                {
                    runtimeState.state = kZoneStateWaiting;
                    runtimeState.remainingMinutes = 0;
                    runtimeState.plannedRuntimeSec = 0;
                    runtimeState.startTimeSec = 0;
                    runtimeState.nextTimeSec = 0;
                    runtimeState.nextAmount = 0.0f;

                    uint16_t manualMinutes = settings.manualRuntimeMinutes;
                    if (settings.maxRuntimeMinutes > 0 && manualMinutes > settings.maxRuntimeMinutes)
                    {
                        manualMinutes = settings.maxRuntimeMinutes;
                    }
                    manualRuntime[zoneIndex] = manualMinutes;
                    queue[queueCount++] = {zoneIndex, kPriorityManualBonus, true};
                }
            }
            else if (!manualRequested[zoneIndex] && runtimeState.state != kZoneStateActive)
            {
                runtimeState.state = kZoneStateInactive;
                runtimeState.remainingMinutes = 0;
                runtimeState.plannedRuntimeSec = 0;
                runtimeState.startTimeSec = 0;
                runtimeState.nextTimeSec = 0;
                runtimeState.nextAmount = 0.0f;
            }
        }
        else if (runtimeState.state != kZoneStateActive)
        {
            if ((minTempStartBlocked || postFreezeBlocked) && !result.soilOverride)
            {
                runtimeState.state = kZoneStateInactive;
                runtimeState.remainingMinutes = 0;
                runtimeState.plannedRuntimeSec = 0;
                runtimeState.startTimeSec = 0;
                runtimeState.nextTimeSec = 0;
                runtimeState.nextAmount = 0.0f;
            }
            else if (result.action == SmartIrrigation::DecisionAction::Start)
            {
                runtimeState.state = kZoneStateWaiting;
                runtimeState.remainingMinutes = 0;
                runtimeState.plannedRuntimeSec = 0;
                runtimeState.startTimeSec = 0;
                runtimeState.nextTimeSec = 0;
                runtimeState.nextAmount = round2(result.waterDemand);

                const float priority = result.soilOverride
                                           ? kPrioritySoilOverrideBonus
                                           : calculatePriority(settings,
                                                               runtimeState.weekAmount,
                                                               runtimeState.cycles,
                                                               runtimeState.lastTimeSec,
                                                               result,
                                                               nowMinutes,
                                                               nowSec,
                                                               timeValid);
                queue[queueCount++] = {zoneIndex, priority, false};
            }
            else if (result.action == SmartIrrigation::DecisionAction::WaitTimeWindow)
            {
                runtimeState.state = kZoneStateWaiting;
                runtimeState.remainingMinutes = 0;
                runtimeState.plannedRuntimeSec = 0;
                runtimeState.startTimeSec = 0;
                runtimeState.nextTimeSec = 0;
                runtimeState.nextAmount = round2(result.waterDemand);
            }
            else
            {
                runtimeState.state = kZoneStateInactive;
                runtimeState.remainingMinutes = 0;
                runtimeState.plannedRuntimeSec = 0;
                runtimeState.startTimeSec = 0;
                runtimeState.nextTimeSec = 0;
                runtimeState.nextAmount = 0.0f;
            }
        }

        if (runtimeState.state == kZoneStateActive)
        {
            ++activeZones;
        }

        if (result.waterDemand > bestDemand)
        {
            bestDemand = result.waterDemand;
            bestDiag = result;
            bestDiagZone = zoneIndex;
        }
    }

    const uint8_t maxConcurrent = std::min<uint8_t>(ParamSIR_SIR_MaxConcurrent, zoneCount);
    uint8_t freeSlots = maxConcurrent > activeZones ? static_cast<uint8_t>(maxConcurrent - activeZones) : 0;

    if (freeSlots > 0)
    {
        for (uint8_t zoneIndex = 0; zoneIndex < zoneCount && freeSlots > 0; ++zoneIndex)
        {
            ZoneRuntime &runtimeState = zoneRuntime_[zoneIndex];
            if (runtimeState.state != kZoneStatePaused || pauseRequested[zoneIndex])
            {
                continue;
            }

            const uint32_t pauseElapsedSec = nowSec >= runtimeState.pauseStartSec
                                                 ? nowSec - runtimeState.pauseStartSec
                                                 : 0;
            const uint32_t pauseMinutes = pauseElapsedSec / 60;
            if (pauseMinutes > 60)
            {
                runtimeState.state = kZoneStateInactive;
                runtimeState.errorActive = true;
                runtimeState.errorCode = 11;
                runtimeState.remainingMinutes = 0;
                runtimeState.plannedRuntimeSec = 0;
                runtimeState.startTimeSec = 0;
                runtimeState.nextTimeSec = 0;
                runtimeState.nextAmount = 0.0f;
                runtimeState.manualRun = false;
                runtimeState.pauseRemainingMinutes = 0;
                runtimeState.pauseStartSec = 0;
                continue;
            }

            runtimeState.remainingMinutes = runtimeState.pauseRemainingMinutes;
            runtimeState.plannedRuntimeSec = static_cast<uint32_t>(runtimeState.pauseRemainingMinutes) * 60;
            runtimeState.startTimeSec = nowSec;
            runtimeState.nextTimeSec = timeValid ? nowSec + runtimeState.plannedRuntimeSec : 0;
            runtimeState.state = kZoneStateActive;
            runtimeState.pauseRemainingMinutes = 0;
            runtimeState.pauseStartSec = 0;
            knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoValve)).value(true, DPT_Switch);
            ++activeZones;
            --freeSlots;
        }
    }

    if (freeSlots > 0 && queueCount > 0)
    {
        std::sort(queue.begin(), queue.begin() + queueCount, [](const QueueEntry &a, const QueueEntry &b) {
            return a.priority > b.priority;
        });

        uint8_t startedZones = 0;
        for (uint8_t entryIndex = 0; entryIndex < queueCount && startedZones < freeSlots; ++entryIndex)
        {
            const uint8_t zoneIndex = queue[entryIndex].zoneIndex;
            ZoneRuntime &runtimeState = zoneRuntime_[zoneIndex];
            if (runtimeState.state != kZoneStateWaiting)
            {
                continue;
            }

            const SmartIrrigation::ZoneSettings &settings = settingsCache[zoneIndex];
            const SmartIrrigation::DecisionResult &result = decisions[zoneIndex];
            const bool isManual = queue[entryIndex].manual;

            if (!isManual && settings.windowType == SmartIrrigation::TimeWindowType::Fixed)
            {
                const bool fixMatch = (settings.window1.active && isFixedWindowMatch(nowMinutes, settings.window1.startMinutes)) ||
                                      (settings.window2.active && isFixedWindowMatch(nowMinutes, settings.window2.startMinutes));
                if (!fixMatch)
                {
                    runtimeState.errorActive = true;
                    runtimeState.errorCode = 9;
                    runtimeState.state = kZoneStateInactive;
                    runtimeState.nextTimeSec = 0;
                    runtimeState.nextAmount = 0.0f;
                    continue;
                }
            }

            const uint16_t manualMinutes = manualRuntime[zoneIndex];
            const char *source = isManual ? "Manuell" : (result.soilOverride ? "Bodenfeuchte" : "Automatisch");
            const uint16_t runtimeOverride = isManual ? manualMinutes : 0;
            // Phase 4.1: Inter-Zone delay - wait for delay if a zone recently stopped
            const uint8_t zoneSwitchDelaySec = ParamSIR_SIR_ZoneSwitchDelay;
            if (zoneSwitchDelaySec > 0 && lastZoneStopMs_ > 0 &&
                (nowMs - lastZoneStopMs_) < static_cast<uint32_t>(zoneSwitchDelaySec) * 1000UL)
            {
                break; // Wait for inter-zone delay before starting next zone
            }
            if (startZone(zoneIndex, settings, result.waterDemand, runtimeOverride, source, nowSec, timeValid))
            {
                runtimeState.manualRun = isManual;
                ++startedZones;
                ++activeZones;
            }
            else if (runtimeState.state != kZoneStateActive)
            {
                runtimeState.state = kZoneStateInactive;
                runtimeState.manualRun = false;
            }
        }
    }

    for (uint8_t zoneIndex = 0; zoneIndex < zoneCount; ++zoneIndex)
    {
        updateZoneOutputs(zoneIndex,
                          decisions[zoneIndex],
                          settingsCache[zoneIndex],
                          zoneRuntime_[zoneIndex],
                          timeValid);
    }

    knx.getGroupObject(singleKoNumber(kKoActiveZones)).value(activeZones, DPT_Value_1_Ucount);
    knx.getGroupObject(singleKoNumber(kKoSystemStatus)).value(systemOn && !emergencyStop, DPT_Switch);
    knx.getGroupObject(singleKoNumber(kKoSystemError)).value(!sensorOk, DPT_Switch);
    knx.getGroupObject(singleKoNumber(kKoDiagFallback)).value(fallbackActiveAny, DPT_Switch);

    updateDiagOutputs(bestDiag, bestDiagZone);

    // 1.1 Check-Back loop: verify valve feedback for all active zones
    for (uint8_t zoneIndex = 0; zoneIndex < zoneCount; ++zoneIndex)
    {
        ZoneRuntime &rt = zoneRuntime_[zoneIndex];
        if (rt.state != kZoneStateActive)
        {
            continue;
        }
        const SmartIrrigation::ZoneSettings &settings = settingsCache[zoneIndex];
        if (!settings.checkBackEnabled || rt.checkBackTimer == 0)
        {
            continue;
        }
        if (nowSec < rt.checkBackTimer)
        {
            continue;
        }
        // Check-Back timer expired \u2014 read feedback KO
        const bool expectedValve = true;
        const bool feedbackKo = knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoValveFeedback)).value(DPT_Switch);
        // AD-7: first check if we ever received a value
        if (!rt.valveFeedbackReceived)
        {
            // No GA assigned or empty \u2014 skip check, disable timer
            rt.checkBackTimer = 0;
            continue;
        }
        if (feedbackKo != expectedValve)
        {
            ++rt.checkBackRetries;
            if (rt.checkBackRetries >= kCheckBackMaxRetries)
            {
                rt.errorActive = true;
                rt.errorCode = kErrorCodeValveMismatch;
                stopZone(zoneIndex, settings, nowSec, timeValid);
                logWarningP("Check-Back Zone %u: Mismatch, Zone gestoppt", static_cast<unsigned>(zoneIndex + 1));
            }
            else
            {
                rt.checkBackTimer = nowSec + settings.checkBackDelaySec;
            }
        }
        else
        {
            rt.checkBackTimer = 0;
            rt.checkBackRetries = 0;
        }
    }

    // 1.2 Flow check (system-wide) \u2014 placeholder for AD-2 implementation
    // (Not fully wired since 1.2 is optional and requires additional params in share.xml)
}

uint16_t SmartIrrigationModule::flashSize()
{
    // V4: version(1) + zoneCount(1) + zones * (weekAmount(4) + weekAmountPrev(4) + cycles(1) + lastIrrigationDayOfYear(2) + windowStartDayOfYear(2)) + suspendEndTime(4)
    return 2 + (kMaxZones * (sizeof(float) * 2 + sizeof(uint8_t) + sizeof(uint16_t) * 2)) + sizeof(uint32_t);
}

void SmartIrrigationModule::writeFlash()
{
    openknx.flash.writeByte(kFlashVersion); // V4
    openknx.flash.writeByte(kMaxZones);

    for (const auto &zone : zoneRuntime_)
    {
        openknx.flash.writeFloat(zone.weekAmount);
        openknx.flash.writeFloat(zone.weekAmountPrev);
        openknx.flash.writeByte(zone.cycles);
        openknx.flash.writeWord(zone.lastIrrigationDayOfYear);
        openknx.flash.writeWord(zone.windowStartDayOfYear);
    }

    // V3: Suspend end time (AD-6)
    openknx.flash.writeInt(suspendEndTime_);
}

void SmartIrrigationModule::readFlash(const uint8_t *data, const uint16_t size)
{
    (void)data;
    if (size == 0)
    {
        return;
    }

    const uint8_t version = openknx.flash.readByte();
    const uint8_t zoneCount = openknx.flash.readByte();
    (void)zoneCount;

    if (version == 1)
    {
        // V1 format: zones * (weekAmount(4) + weekAmountPrev(4) + cycles(1))
        const uint16_t v1ZoneEntrySize = static_cast<uint16_t>(sizeof(float) * 2 + sizeof(uint8_t));
        const uint8_t availableZones = size > 2 ? static_cast<uint8_t>((size - 2) / v1ZoneEntrySize) : 0;
        for (uint8_t index = 0; index < availableZones; ++index)
        {
            const float weekAmount = openknx.flash.readFloat();
            const float weekAmountPrev = openknx.flash.readFloat();
            const uint8_t cycles = openknx.flash.readByte();

            if (index < kMaxZones)
            {
                zoneRuntime_[index].weekAmount = weekAmount;
                zoneRuntime_[index].weekAmountPrev = weekAmountPrev;
                zoneRuntime_[index].cycles = cycles;
                zoneRuntime_[index].lastIrrigationDayOfYear = 0; // Default for V1→V2 migration
            }
        }
        logDebugP("SmartIrrigation flash V1→V2 migration completed");
    }
    else if (version == 2)
    {
        // V2 format: zones * (weekAmount(4) + weekAmountPrev(4) + cycles(1) + lastIrrigationDayOfYear(2))
        const uint16_t v2ZoneEntrySize = static_cast<uint16_t>(sizeof(float) * 2 + sizeof(uint8_t) + sizeof(uint16_t));
        const uint8_t availableZones = size > 2 ? static_cast<uint8_t>((size - 2) / v2ZoneEntrySize) : 0;
        for (uint8_t index = 0; index < availableZones; ++index)
        {
            const float weekAmount = openknx.flash.readFloat();
            const float weekAmountPrev = openknx.flash.readFloat();
            const uint8_t cycles = openknx.flash.readByte();
            const uint16_t lastIrrigationDayOfYear = openknx.flash.readWord();

            if (index < kMaxZones)
            {
                zoneRuntime_[index].weekAmount = weekAmount;
                zoneRuntime_[index].weekAmountPrev = weekAmountPrev;
                zoneRuntime_[index].cycles = cycles;
                zoneRuntime_[index].lastIrrigationDayOfYear = lastIrrigationDayOfYear;
            }
        }
        suspendEndTime_ = 0; // V2→V3: no suspend stored
        logDebugP("SmartIrrigation flash V2→V3 migration completed");
    }
    else if (version == 3)
    {
        // V3 format: zones * (weekAmount(4) + weekAmountPrev(4) + cycles(1) + lastIrrigationDayOfYear(2)) + suspendEndTime(4)
        const uint16_t v3ZoneEntrySize = static_cast<uint16_t>(sizeof(float) * 2 + sizeof(uint8_t) + sizeof(uint16_t));
        const uint8_t availableZones = size > (2 + sizeof(uint32_t)) ? static_cast<uint8_t>((size - 2 - sizeof(uint32_t)) / v3ZoneEntrySize) : 0;
        for (uint8_t index = 0; index < availableZones; ++index)
        {
            const float weekAmount = openknx.flash.readFloat();
            const float weekAmountPrev = openknx.flash.readFloat();
            const uint8_t cycles = openknx.flash.readByte();
            const uint16_t lastIrrigationDayOfYear = openknx.flash.readWord();

            if (index < kMaxZones)
            {
                zoneRuntime_[index].weekAmount = weekAmount;
                zoneRuntime_[index].weekAmountPrev = weekAmountPrev;
                zoneRuntime_[index].cycles = cycles;
                zoneRuntime_[index].lastIrrigationDayOfYear = lastIrrigationDayOfYear;
            }
        }
        suspendEndTime_ = openknx.flash.readInt();
        // Restore suspendActive_ from persisted suspendEndTime_
        if (suspendEndTime_ > 0)
        {
            suspendActive_ = true;
            knx.getGroupObject(singleKoNumber(kKoSuspendActive)).value(true, DPT_Switch);
            logInfoP("Suspend restored from flash until %u", suspendEndTime_);
        }
        logDebugP("SmartIrrigation flash V3\u2192V4 migration completed");
    }
    else if (version == 4)
    {
        // V4 format: zones * (weekAmount(4) + weekAmountPrev(4) + cycles(1) + lastIrrigationDayOfYear(2) + windowStartDayOfYear(2)) + suspendEndTime(4)
        const uint16_t v4ZoneEntrySize = static_cast<uint16_t>(sizeof(float) * 2 + sizeof(uint8_t) + sizeof(uint16_t) * 2);
        const uint8_t availableZones = size > (2 + sizeof(uint32_t)) ? static_cast<uint8_t>((size - 2 - sizeof(uint32_t)) / v4ZoneEntrySize) : 0;
        for (uint8_t index = 0; index < availableZones; ++index)
        {
            const float weekAmount = openknx.flash.readFloat();
            const float weekAmountPrev = openknx.flash.readFloat();
            const uint8_t cycles = openknx.flash.readByte();
            const uint16_t lastIrrigationDayOfYear = openknx.flash.readWord();
            const uint16_t windowStartDayOfYear = openknx.flash.readWord();

            if (index < kMaxZones)
            {
                zoneRuntime_[index].weekAmount = weekAmount;
                zoneRuntime_[index].weekAmountPrev = weekAmountPrev;
                zoneRuntime_[index].cycles = cycles;
                zoneRuntime_[index].lastIrrigationDayOfYear = lastIrrigationDayOfYear;
                zoneRuntime_[index].windowStartDayOfYear = windowStartDayOfYear;
            }
        }
        suspendEndTime_ = openknx.flash.readInt();
        if (suspendEndTime_ > 0)
        {
            suspendActive_ = true;
            knx.getGroupObject(singleKoNumber(kKoSuspendActive)).value(true, DPT_Switch);
            logInfoP("Suspend restored from flash until %u", suspendEndTime_);
        }
    }
    else
    {
        // Unknown version > 4: try to read V4 fields, skip rest
        logDebugP("SmartIrrigation flash unknown version (%u), attempting V4 read", version);
        const uint16_t v4ZoneEntrySize = static_cast<uint16_t>(sizeof(float) * 2 + sizeof(uint8_t) + sizeof(uint16_t) * 2);
        const uint8_t availableZones = size > (2 + sizeof(uint32_t)) ? static_cast<uint8_t>((size - 2 - sizeof(uint32_t)) / v4ZoneEntrySize) : 0;
        for (uint8_t index = 0; index < availableZones; ++index)
        {
            const float weekAmount = openknx.flash.readFloat();
            const float weekAmountPrev = openknx.flash.readFloat();
            const uint8_t cycles = openknx.flash.readByte();
            const uint16_t lastIrrigationDayOfYear = openknx.flash.readWord();
            const uint16_t windowStartDayOfYear = openknx.flash.readWord();

            if (index < kMaxZones)
            {
                zoneRuntime_[index].weekAmount = weekAmount;
                zoneRuntime_[index].weekAmountPrev = weekAmountPrev;
                zoneRuntime_[index].cycles = cycles;
                zoneRuntime_[index].lastIrrigationDayOfYear = lastIrrigationDayOfYear;
                zoneRuntime_[index].windowStartDayOfYear = windowStartDayOfYear;
            }
        }
        suspendEndTime_ = openknx.flash.readInt();
        if (suspendEndTime_ > 0)
        {
            suspendActive_ = true;
            knx.getGroupObject(singleKoNumber(kKoSuspendActive)).value(true, DPT_Switch);
        }
    }
}

void SmartIrrigationModule::saveRuntimeToFlash(bool force)
{
    openknx.flash.save(force);
}

#if (MASK_VERSION & 0x0900) != 0x0900 // Coupler do not have GroupObjects
void SmartIrrigationModule::processInputKo(GroupObject &ko)
{
    OpenKNX::Module::processInputKo(ko);

    const uint16_t koNumber = ko.asap();
    if (koNumber < kKoSingleOffset)
    {
        return;
    }
    const uint16_t koIndex = koNumber - kKoSingleOffset + 1;
    // Accept KOs in range 1..kKoSingleCount (low block) and 400+ (high block)
    if (koIndex > kKoSingleCount && koIndex < 400)
    {
        return;
    }

    switch (koIndex)
    {
        case kKoFlowSensor:
        {
            // 1.2 Leckage-Erkennung: speichere Durchfluss und Zeitstempel
            const float value = ko.value(DPT_Value_Volume_Flow);
            if (isValidRange(value, 0.0f, 1000.0f))
            {
                currentFlowLpm_ = value;
                flowLastUpdateMs_ = millis();
            }
            break;
        }
        case kKoAdjustmentFactor:
        {
            // 2.5 Externer Korrekturfaktor: 0-200% (DPT_Scaling = 0-255 → 0-100%)
            const uint8_t rawValue = ko.value(DPT_Scaling);
            // DPT_Scaling is 0-255 representing 0-100%. We want 0-200%.
            // We interpret the raw value directly as percentage (0-200 clamped).
            adjustmentFactorPercent_ = (rawValue > 200) ? 200 : rawValue;
            break;
        }
        case kKoSensorTemperature:
        {
            const float value = ko.value(DPT_Value_Temp);
            const bool valid = isValidRange(value, -40.0f, 60.0f);
            weatherCache_.real.hasTemperature = valid;
            if (valid)
            {
                weatherCache_.real.temperatureC = value;
                sensorLastValidMs_[static_cast<size_t>(SmartIrrigation::SensorId::Temperature)] = millis();
            }
            sensorHealth_[static_cast<size_t>(SmartIrrigation::SensorId::Temperature)].update(valid);
            break;
        }
        case kKoSensorRain:
        {
            const bool value = ko.value(DPT_Switch);
            weatherCache_.real.hasRain = true;
            weatherCache_.real.rainActive = value;
            sensorLastValidMs_[static_cast<size_t>(SmartIrrigation::SensorId::Rain)] = millis();
            sensorHealth_[static_cast<size_t>(SmartIrrigation::SensorId::Rain)].update(true);
            if (value && weatherCache_.real.hasRainAmount)
            {
                updateRainLockFromEvent(weatherCache_.real.rainAmountMm, maxRainDelayFactor());
            }
            break;
        }
        case kKoSensorRainAmount:
        {
            const float value = ko.value(DPT_Rain_Amount);
            const bool valid = isValidRange(value, 0.0f, 200.0f);
            weatherCache_.real.hasRainAmount = valid;
            if (valid)
            {
                weatherCache_.real.rainAmountMm = value;
                sensorLastValidMs_[static_cast<size_t>(SmartIrrigation::SensorId::RainAmount)] = millis();
            }
            sensorHealth_[static_cast<size_t>(SmartIrrigation::SensorId::RainAmount)].update(valid);
            if (valid && weatherCache_.real.rainActive)
            {
                updateRainLockFromEvent(value, maxRainDelayFactor());
            }
            break;
        }
        case kKoWeatherUpdateTrigger:
        {
            const bool trigger = ko.value(DPT_Switch);
            if (!trigger)
            {
                break;
            }

            const bool realAvailable = weatherCache_.real.hasTemperature && weatherCache_.real.hasRain;
            const bool updateOk = realAvailable || weatherCache_.forecast.hasEt0Day[0];
            knx.getGroupObject(singleKoNumber(kKoWeatherUpdateStatus)).value(updateOk, DPT_Switch);

            if (openknx.time.isValid())
            {
                auto currentTime = openknx.time.getLocalTime();
                weatherUpdateLastSec_ = static_cast<uint32_t>(currentTime.toTime_t());
                writeDateTimeKo(singleKoNumber(kKoWeatherUpdateLast), weatherUpdateLastSec_, true);
            }
            break;
        }
        case kKoSensorWind:
        {
            const float value = ko.value(DPT_Value_Wsp);
            const bool valid = isValidRange(value, 0.0f, 50.0f);
            weatherCache_.real.hasWind = valid;
            if (valid)
            {
                weatherCache_.real.windSpeedMs = value;
            }
            sensorHealth_[static_cast<size_t>(SmartIrrigation::SensorId::Wind)].update(valid);
            break;
        }
        // ET0 daily KOs (Tag 0-6): stored in array for weekly planning
        case kKoForecastET0Today:    // Tag 0
        case kKoForecastET0Tomorrow: // Tag 1
        case kKoForecastET0Day2:
        case kKoForecastET0Day3:
        case kKoForecastET0Day4:
        case kKoForecastET0Day5:
        case kKoForecastET0Day6:
        {
            static constexpr uint16_t et0KoMap[] = {
                kKoForecastET0Today, kKoForecastET0Tomorrow,
                kKoForecastET0Day2, kKoForecastET0Day3, kKoForecastET0Day4,
                kKoForecastET0Day5, kKoForecastET0Day6
            };
            const float value = ko.value(DPT_Rain_Amount);
            const bool valid = isValidRange(value, 0.0f, 20.0f);
            for (uint8_t i = 0; i < 7; ++i)
            {
                if (et0KoMap[i] == koIndex)
                {
                    weatherCache_.forecast.hasEt0Day[i] = valid;
                    if (valid) weatherCache_.forecast.et0DayMm[i] = value;
                    break;
                }
            }
            break;
        }
        // Rain daily KOs (Tag 0-6): stored in array for weekly planning
        case kKoForecastRainDay0:
        case kKoForecastRainDay1:
        case kKoForecastRainDay2:
        case kKoForecastRainDay3:
        case kKoForecastRainDay4:
        case kKoForecastRainDay5:
        case kKoForecastRainDay6:
        {
            static constexpr uint16_t rainKoMap[] = {
                kKoForecastRainDay0, kKoForecastRainDay1, kKoForecastRainDay2,
                kKoForecastRainDay3, kKoForecastRainDay4, kKoForecastRainDay5,
                kKoForecastRainDay6
            };
            const float value = ko.value(DPT_Rain_Amount);
            const bool valid = isValidRange(value, 0.0f, 200.0f);
            for (uint8_t i = 0; i < 7; ++i)
            {
                if (rainKoMap[i] == koIndex)
                {
                    weatherCache_.forecast.hasRainDay[i] = valid;
                    if (valid) weatherCache_.forecast.rainDayMm[i] = value;
                    break;
                }
            }
            break;
        }
        // Phase 3: Tank Level (3.1)
        case kKoTankLevel:
        {
            const uint8_t value = ko.value(DPT_Scaling);
            tankLevelPercent_ = value;
            break;
        }
        // Phase 4.2: Suspend
        case kKoSuspendHours:
        {
            const uint16_t hours = ko.value(DPT_Value_2_Count);
            if (hours > 0)
            {
                // Set suspend end time (use OpenKNX local time — same source as loop())
                {
                    auto suspendNow = openknx.time.getLocalTime();
                    suspendEndTime_ = static_cast<uint32_t>(suspendNow.toTime_t()) + static_cast<uint32_t>(hours) * 3600UL;
                }
                if (!suspendActive_)
                {
                    suspendActive_ = true;
                    knx.getGroupObject(singleKoNumber(kKoSuspendActive)).value(true, DPT_Switch);
                    logInfoP("Suspend activated for %u hours", hours);
                }
                saveRuntimeToFlash(true); // AD-6: persist suspend across reboot
            }
            else
            {
                // Cancel suspend
                suspendEndTime_ = 0;
                if (suspendActive_)
                {
                    suspendActive_ = false;
                    knx.getGroupObject(singleKoNumber(kKoSuspendActive)).value(false, DPT_Switch);
                    logInfoP("Suspend cancelled");
                    saveRuntimeToFlash(true); // AD-6: persist cancel across reboot
                }
            }
            break;
        }
        // Erstinbetriebnahme: Per-Objekt Freigabe (Modus 2)
        case kKoFirstStartRelease:
        {
            const bool trigger = ko.value(DPT_Switch);
            if (trigger && firstStartBlocked_ && ParamSIR_SIR_FirstStartMode == 2)
            {
                firstStartBlocked_ = false;
                firstStartReleaseAtMs_ = 0;
                knx.getGroupObject(singleKoNumber(kKoFirstStartActive)).value(false, DPT_Switch);
                logInfoP("Erstinbetriebnahme: Freigabe per KO erhalten");
            }
            break;
        }
        default:
            break;
    }
}
#endif

SmartIrrigation::ZoneSettings SmartIrrigationModule::loadZoneSettings(uint8_t zoneIndex)
{
    uint8_t _channelIndex = zoneIndex;
    (void)_channelIndex;

    SmartIrrigation::ZoneSettings settings{};
    settings.enabled = ParamSIR_SIR_ZEnabled;
    settings.areaM2 = ParamSIR_SIR_ZArea;
    settings.isDrip = (ParamSIR_SIR_ZZoneType == 1);
    settings.isSmartPro = (ParamSIR_SIR_Mode == 1);

    if (settings.isDrip)
    {
        // Drip irrigation: total flow in l/h, precip rate computed from area
        settings.dripFlowLph = ParamSIR_SIR_ZDripFlow;
    }
    else if (settings.isSmartPro)
    {
        // Smart Pro: detailed sprinkler configuration
        settings.sprinklerConfig.unit = static_cast<SmartIrrigation::FlowInputUnit>(ParamSIR_SIR_ZFlowInputUnit);
        settings.sprinklerConfig.count = ParamSIR_SIR_ZSprinklerCount;
        if (settings.sprinklerConfig.count > SmartIrrigation::MAX_SPRINKLERS)
            settings.sprinklerConfig.count = SmartIrrigation::MAX_SPRINKLERS;
        if (settings.sprinklerConfig.count >= 1) settings.sprinklerConfig.values[0] = ParamSIR_SIR_ZSprinkler1;
        if (settings.sprinklerConfig.count >= 2) settings.sprinklerConfig.values[1] = ParamSIR_SIR_ZSprinkler2;
        if (settings.sprinklerConfig.count >= 3) settings.sprinklerConfig.values[2] = ParamSIR_SIR_ZSprinkler3;
        if (settings.sprinklerConfig.count >= 4) settings.sprinklerConfig.values[3] = ParamSIR_SIR_ZSprinkler4;
        if (settings.sprinklerConfig.count >= 5) settings.sprinklerConfig.values[4] = ParamSIR_SIR_ZSprinkler5;
        if (settings.sprinklerConfig.count >= 6) settings.sprinklerConfig.values[5] = ParamSIR_SIR_ZSprinkler6;
    }
    else
    {
        // Smart Core: simple precipitation rate from datasheet
        settings.precipRateMmh = ParamSIR_SIR_ZPrecipRate;
    }
    settings.etFactorPercent = ParamSIR_SIR_ZETFactor;
    settings.interceptionPercent = ParamSIR_SIR_ZInterception;
    settings.minWeek = ParamSIR_SIR_ZMinWeek;
    settings.maxWeek = ParamSIR_SIR_ZMaxWeek;
    settings.absMaxWeek = ParamSIR_SIR_ZAbsMax;
    settings.maxCycles = ParamSIR_SIR_ZMaxCycles;
    settings.minPerCycle = ParamSIR_SIR_ZMinPerCycle;
    settings.manualRuntimeMinutes = ParamSIR_SIR_ZManualRuntime;
    settings.baseTempC = ParamSIR_SIR_ZBaseTemp;
    settings.baseTempHyst = ParamSIR_SIR_ZBaseTempHyst;
    settings.soilThresholdPercent = ParamSIR_SIR_ZSoilThreshold;
    settings.soilMoistureEnabled = (ParamSIR_SIR_ZSoilMoistureEnabled != 0);
    settings.rainDelayFactor = ParamSIR_SIR_ZRainDelayFactor;
    settings.maxRuntimeMinutes = ParamSIR_SIR_ZMaxRuntime;
    settings.window1.active = ParamSIR_SIR_ZTW1Active;
    settings.window1.startMinutes = knx.paramWord(SIR_ParamCalcIndex(SIR_SIR_ZTW1Start));
    settings.window1.endMinutes = knx.paramWord(SIR_ParamCalcIndex(SIR_SIR_ZTW1End));
    settings.window2.active = ParamSIR_SIR_ZTW2Active;
    settings.window2.startMinutes = knx.paramWord(SIR_ParamCalcIndex(SIR_SIR_ZTW2Start));
    settings.window2.endMinutes = knx.paramWord(SIR_ParamCalcIndex(SIR_SIR_ZTW2End));
    // TimeWindowType: 0=Flexible, 1=Fixed, 2=SunriseBased, 3=SunsetBased
    switch (ParamSIR_SIR_ZTimeType)
    {
        case 0:
            settings.windowType = SmartIrrigation::TimeWindowType::Flexible;
            break;
        case 1:
            settings.windowType = SmartIrrigation::TimeWindowType::Fixed;
            break;
        case 2:
            settings.windowType = SmartIrrigation::TimeWindowType::SunriseBased;
            break;
        case 3:
            settings.windowType = SmartIrrigation::TimeWindowType::SunsetBased;
            break;
        default:
            settings.windowType = SmartIrrigation::TimeWindowType::Flexible;
            break;
    }
    // Phase 1: Check-Back
    settings.checkBackEnabled = ParamSIR_SIR_ZCheckBack;
    settings.checkBackDelaySec = ParamSIR_SIR_ZCheckBackDelay;
    // Phase 2: Sunrise/Sunset (2.1)
    settings.sunOffsetMinutes = ParamSIR_SIR_ZSunOffset;
    settings.sunWindowDurationMinutes = ParamSIR_SIR_ZSunDuration;
    // Phase 2: Soak-Time (2.2)
    settings.soakTimeMinutes = ParamSIR_SIR_ZSoakTime;
    // Phase 2: Rest Days (2.3)
    settings.irrigationIntervalDays = ParamSIR_SIR_ZRestDays;
    // Phase 3: Zone type + Weekday filter (3.2, 3.3, 3.5)
    settings.isSprinkler = !settings.isDrip;
    // Read weekday bitmask as whole byte (7 individual bits at offset 49)
    settings.allowedWeekdays = knx.paramByte(SIR_ParamCalcIndex(SIR_SIR_ZAllowedMo));
    // Phase 4.3: Post-Irrigation Verify
    settings.verifyEnabled = ParamSIR_SIR_ZVerifyEnabled;
    settings.verifyDelayMinutes = ParamSIR_SIR_ZVerifyDelayMinutes;
    settings.verifyMinDeltaPercent = ParamSIR_SIR_ZVerifyMinDeltaPercent;
    // Bucket model & Lead Time
    settings.soilType = ParamSIR_SIR_ZSoilType;
    settings.maxBucketMm = ParamSIR_SIR_ZMaxBucket;
    settings.drainageRateRaw = ParamSIR_SIR_ZDrainageRate;
    settings.leadTimeSeconds = ParamSIR_SIR_ZLeadTime;
    settings.zonePriority = ParamSIR_SIR_ZZonePriority;
    settings.activityEnabled = (ParamSIR_SIR_ZActivityEnabled != 0);

    return settings;
}

void SmartIrrigationModule::updateZoneOutputs(uint8_t zoneIndex,
                                              const SmartIrrigation::DecisionResult &result,
                                              const SmartIrrigation::ZoneSettings &settings,
                                              const ZoneRuntime &runtime,
                                              bool timeValid)
{
    (void)settings;

    const uint16_t koState = zoneKoNumber(zoneIndex, kZoneKoState);
    const uint16_t koValve = zoneKoNumber(zoneIndex, kZoneKoValve);
    const uint16_t koRemaining = zoneKoNumber(zoneIndex, kZoneKoRemaining);
    const uint16_t koWeekAmount = zoneKoNumber(zoneIndex, kZoneKoWeekAmount);
    const uint16_t koWeekAmountPrev = zoneKoNumber(zoneIndex, kZoneKoWeekAmountPrev);
    const uint16_t koLastTime = zoneKoNumber(zoneIndex, kZoneKoLastTime);
    const uint16_t koLastAmount = zoneKoNumber(zoneIndex, kZoneKoLastAmount);
    const uint16_t koNextTime = zoneKoNumber(zoneIndex, kZoneKoNextTime);
    const uint16_t koNextAmount = zoneKoNumber(zoneIndex, kZoneKoNextAmount);
    const uint16_t koErrorCode = zoneKoNumber(zoneIndex, kZoneKoErrorCode);
    const uint16_t koCycles = zoneKoNumber(zoneIndex, kZoneKoCycles);

    const bool isActive = runtime.state == kZoneStateActive;
    knx.getGroupObject(koState).value(runtime.state, DPT_Value_1_Ucount);
    knx.getGroupObject(koValve).value(isActive, DPT_Switch);

    knx.getGroupObject(koRemaining).value(minutesToHoursCeil(runtime.remainingMinutes), DPT_TimePeriodHrs);

    knx.getGroupObject(koWeekAmount).value(runtime.weekAmount, DPT_Rain_Amount);
    knx.getGroupObject(koWeekAmountPrev).value(runtime.weekAmountPrev, DPT_Rain_Amount);
    knx.getGroupObject(koCycles).value(runtime.cycles, DPT_Value_1_Ucount);
    knx.getGroupObject(koLastAmount).value(runtime.lastAmount, DPT_Rain_Amount);
    knx.getGroupObject(koNextAmount).value(runtime.nextAmount, DPT_Rain_Amount);

    if (runtime.lastTimeSec > 0)
    {
        writeDateTimeKo(koLastTime, runtime.lastTimeSec, timeValid);
    }
    if (runtime.nextTimeSec > 0)
    {
        writeDateTimeKo(koNextTime, runtime.nextTimeSec, timeValid);
    }

    uint8_t errorCode = runtime.errorActive ? runtime.errorCode : 0;
    if (errorCode == 0 && result.timeWindowConflict)
    {
        errorCode = 7;
    }
    knx.getGroupObject(koErrorCode).value(errorCode, DPT_Value_1_Ucount);
}

bool SmartIrrigationModule::startZone(uint8_t zoneIndex,
                                      const SmartIrrigation::ZoneSettings &settings,
                                      float waterDemand,
                                      uint16_t manualRuntimeMinutes,
                                      const char *source,
                                      uint32_t nowSec,
                                      bool timeValid)
{
    ZoneRuntime &runtime = zoneRuntime_[zoneIndex];

    const float precipRate = SmartIrrigation::getEffectivePrecipRate(settings);
    float waterDemandM2 = waterDemand;
    float runtimeMinutes = 0.0f;
    const float leadTimeMinutes = static_cast<float>(settings.leadTimeSeconds) / 60.0f;
    if (manualRuntimeMinutes > 0)
    {
        runtimeMinutes = static_cast<float>(manualRuntimeMinutes);
        if (precipRate > 0.0f)
        {
            // Manual runtime is total valve-open time including lead time (Bug #4)
            const float irrigationMinutes = std::max(0.0f, runtimeMinutes - leadTimeMinutes);
            waterDemandM2 = (irrigationMinutes / 60.0f) * precipRate;
        }
    }
    else
    {
        // Calculate irrigation time uncapped, add lead time, then cap total at maxRuntime (Bug #4)
        runtimeMinutes = SmartIrrigation::calculateRuntimeMinutes(precipRate, waterDemandM2, 0);
        runtimeMinutes += leadTimeMinutes;
        if (settings.maxRuntimeMinutes > 0 && runtimeMinutes > static_cast<float>(settings.maxRuntimeMinutes))
        {
            runtimeMinutes = static_cast<float>(settings.maxRuntimeMinutes);
        }
    }

    runtimeMinutes = std::round(runtimeMinutes);

    if (runtimeMinutes <= 0.0f)
    {
        runtime.errorActive = true;
        runtime.errorCode = 8;
        logErrorP("StarteZone Zone %u fehlgeschlagen: Ungueltige Laufzeit (%.2f min)",
                  static_cast<unsigned>(zoneIndex + 1), runtimeMinutes);
        return false;
    }

    if (waterDemandM2 <= 0.0f)
    {
        logWarningP("StarteZone Zone %u uebersprungen: Kein Wasserbedarf", static_cast<unsigned>(zoneIndex + 1));
        return false;
    }

    if (settings.absMaxWeek > 0)
    {
        const float remaining = static_cast<float>(settings.absMaxWeek) - runtime.weekAmount;
        if (waterDemandM2 > remaining)
        {
            waterDemandM2 = remaining;
            runtimeMinutes = SmartIrrigation::calculateRuntimeMinutes(precipRate,
                                                                     waterDemandM2,
                                                                     settings.maxRuntimeMinutes);
            runtimeMinutes = std::round(runtimeMinutes);
            if (runtimeMinutes <= 0.0f)
            {
                logWarningP("StarteZone Zone %u uebersprungen: Wochenlimit erreicht",
                            static_cast<unsigned>(zoneIndex + 1));
                return false;
            }
            logInfoP("StarteZone Zone %u: Laufzeit reduziert auf %.0f min (Wochenlimit)",
                     static_cast<unsigned>(zoneIndex + 1), runtimeMinutes);
        }
    }

    runtime.state = kZoneStateActive;
    runtime.startTimeSec = nowSec;
    runtime.plannedRuntimeSec = static_cast<uint32_t>(runtimeMinutes * 60.0f);
    runtime.remainingMinutes = static_cast<uint16_t>(runtimeMinutes);
    runtime.nextTimeSec = timeValid ? nowSec + runtime.plannedRuntimeSec : 0;
    runtime.nextAmount = round2(waterDemandM2);
    runtime.errorActive = false;
    runtime.errorCode = 0;

    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoValve)).value(true, DPT_Switch);
    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoState)).value(kZoneStateActive, DPT_Value_1_Ucount);
    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoRemaining)).value(minutesToHoursCeil(runtime.remainingMinutes),
                                                                       DPT_TimePeriodHrs);
    if (runtime.nextTimeSec > 0)
    {
        writeDateTimeKo(zoneKoNumber(zoneIndex, kZoneKoNextTime), runtime.nextTimeSec, timeValid);
    }
    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoNextAmount)).value(runtime.nextAmount, DPT_Rain_Amount);

    // 1.1 Check-Back: start timer if feature enabled for this zone
    if (settings.checkBackEnabled)
    {
        runtime.checkBackTimer = nowSec + settings.checkBackDelaySec;
        runtime.checkBackRetries = 0;
    }
    else
    {
        runtime.checkBackTimer = 0;
        runtime.checkBackRetries = 0;
    }

    // Phase 4.3: Capture initial soil moisture for post-irrigation verify
    if (settings.verifyEnabled)
    {
        // Read current soil moisture from weather cache
        runtime.verifyStartSoil = hasZoneSoilMoisture_[zoneIndex]
                                       ? static_cast<uint8_t>(zoneSoilMoisturePercent_[zoneIndex])
                                       : 0;
        runtime.verifyTimerMs = 0;  // Will be set in stopZone()
    }
    else
    {
        runtime.verifyStartSoil = 0;
        runtime.verifyTimerMs = 0;
    }

    logInfoP("Zone %u gestartet: Quelle=%s, Laufzeit=%.0f min, Menge=%.2f l/m2",
             static_cast<unsigned>(zoneIndex + 1), source, runtimeMinutes, runtime.nextAmount);
    return true;
}

bool SmartIrrigationModule::stopZone(uint8_t zoneIndex,
                                     const SmartIrrigation::ZoneSettings &settings,
                                     uint32_t nowSec,
                                     bool timeValid)
{
    ZoneRuntime &runtime = zoneRuntime_[zoneIndex];
    if (runtime.state != kZoneStateActive)
    {
        return false;
    }

    const uint32_t elapsedSec = nowSec >= runtime.startTimeSec ? nowSec - runtime.startTimeSec : 0;
    const float elapsedMinutes = static_cast<float>(elapsedSec) / 60.0f;
    float actualAmountM2 = 0.0f;
    {
        const float precipRate = SmartIrrigation::getEffectivePrecipRate(settings);
        // Subtract lead time: only actual irrigation time contributes to water delivered (Bug #3)
        const float leadTimeMinutes = static_cast<float>(settings.leadTimeSeconds) / 60.0f;
        const float irrigationMinutes = std::max(0.0f, elapsedMinutes - leadTimeMinutes);
        actualAmountM2 = precipRate * (irrigationMinutes / 60.0f);
    }
    actualAmountM2 = round2(actualAmountM2);

    runtime.weekAmount += actualAmountM2;
    runtime.cycles += 1;
    runtime.lastTimeSec = nowSec;
    runtime.lastAmount = actualAmountM2;
    // Phase 2.2: Check if zone should enter Soaking instead of Inactive
    // Conditions: not manual run, not last cycle, soaking enabled
    const bool shouldSoak = !runtime.manualRun &&
                            runtime.cycles < settings.maxCycles &&
                            settings.soakTimeMinutes > 0;
    if (shouldSoak)
    {
        runtime.state = kZoneStateSoaking;
        runtime.soakEndSec = nowSec + static_cast<uint32_t>(settings.soakTimeMinutes) * 60;
    }
    else
    {
        runtime.state = kZoneStateInactive;
        runtime.soakEndSec = 0;
    }
    // Phase 2.3: Record last irrigation day; set window start on first cycle of interval
    if (timeValid)
    {
        const auto localTime = openknx.time.getLocalTime();
        runtime.lastIrrigationDayOfYear = calculateDayOfYear(localTime.month, localTime.day);
        if (runtime.windowStartDayOfYear == 0)
        {
            runtime.windowStartDayOfYear = runtime.lastIrrigationDayOfYear;
        }
    }
    runtime.errorActive = false;
    runtime.errorCode = 0;
    runtime.remainingMinutes = 0;
    runtime.plannedRuntimeSec = 0;
    runtime.nextTimeSec = 0;
    runtime.nextAmount = 0.0f;
    runtime.manualRun = false;
    runtime.pauseRemainingMinutes = 0;
    runtime.pauseStartSec = 0;
    runtime.checkBackTimer = 0;
    runtime.checkBackRetries = 0;

    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoValve)).value(false, DPT_Switch);
    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoState)).value(runtime.state, DPT_Value_1_Ucount);
    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoRemaining)).value(static_cast<uint16_t>(0), DPT_TimePeriodHrs);
    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoWeekAmount)).value(runtime.weekAmount, DPT_Rain_Amount);
    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoCycles)).value(runtime.cycles, DPT_Value_1_Ucount);
    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoLastAmount)).value(runtime.lastAmount, DPT_Rain_Amount);
    if (runtime.lastTimeSec > 0)
    {
        writeDateTimeKo(zoneKoNumber(zoneIndex, kZoneKoLastTime), runtime.lastTimeSec, timeValid);
    }

    // Phase 4.1: Track last zone stop time for inter-zone delay
    lastZoneStopMs_ = millis();

    // Phase 4.3: Start post-irrigation verify timer if enabled
    // verifyStartSoil was set in startZone()
    if (runtime.verifyStartSoil > 0)
    {
        // Timer will be checked in loop()
        runtime.verifyTimerMs = millis();
    }

    saveRuntimeToFlash();

    logInfoP("Zone %u gestoppt: Laufzeit=%.2f min, Menge=%.2f l/m2, Wochenmenge=%.2f l/m2, Zyklen=%u",
             static_cast<unsigned>(zoneIndex + 1),
             elapsedMinutes,
             runtime.lastAmount,
             runtime.weekAmount,
             static_cast<unsigned>(runtime.cycles));
    return true;
}

uint8_t SmartIrrigationModule::stopAllZones(uint8_t zoneCount, uint32_t nowSec, bool timeValid)
{
    uint8_t stopped = 0;
    const uint8_t boundedZoneCount = std::min<uint8_t>(zoneCount, kMaxZones);
    for (uint8_t zoneIndex = 0; zoneIndex < boundedZoneCount; ++zoneIndex)
    {
        ZoneRuntime &runtimeState = zoneRuntime_[zoneIndex];
        if (runtimeState.state == kZoneStateInactive)
        {
            continue;
        }
        if (runtimeState.state == kZoneStateActive)
        {
            SmartIrrigation::ZoneSettings settings = loadZoneSettings(zoneIndex);
            if (stopZone(zoneIndex, settings, nowSec, timeValid))
            {
                // BUG-FIX EC-11: Set error code for emergency-stopped zones
                runtimeState.errorActive = true;
                runtimeState.errorCode = 12;  // Emergency-Stop
                ++stopped;
            }
        }
        else if (runtimeState.state == kZoneStatePaused)
        {
            // Paused: Ventil bereits zu, aber State und Timer aufräumen
            runtimeState.pauseRemainingMinutes = 0;
            runtimeState.pauseStartSec = 0;
            runtimeState.state = kZoneStateInactive;
            runtimeState.manualRun = false;
            // BUG-FIX EC-11: Set error code
            runtimeState.errorActive = true;
            runtimeState.errorCode = 12;  // Emergency-Stop
        }
        else
        {
            // Waiting oder Soaking: Ventil zu, State zurücksetzen
            runtimeState.soakEndSec = 0;
            runtimeState.state = kZoneStateInactive;
            runtimeState.manualRun = false;
            // M-3: Konsistenz - auch Waiting/Soaking erhält errorCode
            runtimeState.errorActive = true;
            runtimeState.errorCode = 12;  // Emergency-Stop
        }
    }

    logWarningP("Notaus: %u Zonen gestoppt", static_cast<unsigned>(stopped));
    return stopped;
}

void SmartIrrigationModule::updateActiveZoneCountdown(uint8_t zoneCount, uint32_t nowSec, bool timeValid)
{
    (void)timeValid;
    const uint8_t boundedZoneCount = std::min<uint8_t>(zoneCount, kMaxZones);
    for (uint8_t zoneIndex = 0; zoneIndex < boundedZoneCount; ++zoneIndex)
    {
        ZoneRuntime &runtime = zoneRuntime_[zoneIndex];
        if (runtime.state != kZoneStateActive)
        {
            continue;
        }

        if (runtime.plannedRuntimeSec == 0 || runtime.startTimeSec == 0)
        {
            runtime.remainingMinutes = 0;
            continue;
        }

        // Freeze countdown when tank-paused: valve closed, no water delivered (Bug #6)
        if (runtime.tankPaused)
        {
            runtime.startTimeSec = nowSec;
            runtime.remainingMinutes = static_cast<uint16_t>((runtime.plannedRuntimeSec + 59) / 60);
            continue;
        }

        const uint32_t elapsedSec = nowSec >= runtime.startTimeSec ? nowSec - runtime.startTimeSec : 0;
        if (elapsedSec >= runtime.plannedRuntimeSec)
        {
            SmartIrrigation::ZoneSettings settings = loadZoneSettings(zoneIndex);
            stopZone(zoneIndex, settings, nowSec, timeValid);
            continue;
        }

        const uint32_t remainingSec = runtime.plannedRuntimeSec - elapsedSec;
        runtime.remainingMinutes = static_cast<uint16_t>((remainingSec + 59) / 60);
    }
}

void SmartIrrigationModule::updateDiagOutputs(const SmartIrrigation::DecisionResult &result,
                                              uint8_t decisionZone)
{
    const bool diagMode = knx.getGroupObject(singleKoNumber(kKoDiagMode)).value(DPT_Switch);
    if (!diagMode)
    {
        return;
    }

    (void)decisionZone;
    knx.getGroupObject(singleKoNumber(kKoDiagEt0)).value(result.et0, DPT_Rain_Amount);
    knx.getGroupObject(singleKoNumber(kKoDiagEffectiveRain)).value(result.effectiveRain, DPT_Rain_Amount);
    knx.getGroupObject(singleKoNumber(kKoDiagWaterDemand)).value(result.waterDemand, DPT_Rain_Amount);
    knx.getGroupObject(singleKoNumber(kKoDiagDecisionMode)).value(static_cast<uint8_t>(result.mode), DPT_Value_1_Ucount);
    knx.getGroupObject(singleKoNumber(kKoDiagSequenceStatus)).value(static_cast<uint8_t>(result.action), DPT_Value_1_Ucount);
}

void SmartIrrigationModule::updateSensorStatusKo(bool sensorOk)
{
    knx.getGroupObject(singleKoNumber(kKoSensorStatus)).value(sensorOk, DPT_Switch);
}

void SmartIrrigationModule::updateRainLockFromEvent(float rainAmountMm, float maxFactor)
{
    if (rainAmountMm <= 0.0f || maxFactor <= 0.0f)
    {
        return;
    }

    rainLock_.remainingHours = SmartIrrigation::calculateRainLockHours(rainAmountMm,
                                                                       maxFactor,
                                                                       rainLock_.remainingHours);
    rainLock_.active = rainLock_.remainingHours > 0.0f;
    rainLock_.lastUpdateMs = millis();
}

void SmartIrrigationModule::updateRainLockCountdown()
{
    const uint32_t nowMs = millis();
    if (rainLock_.lastUpdateMs == 0)
    {
        rainLock_.lastUpdateMs = nowMs;
        return;
    }

    if (rainLock_.remainingHours > 0.0f)
    {
        uint32_t elapsedMs = nowMs - rainLock_.lastUpdateMs; // unsigned subtraction handles wraparound
        if (elapsedMs > 60000)
        {
            elapsedMs = 60000; // cap at 60s to prevent jumps after millis() wraparound
        }
        if (elapsedMs > 0)
        {
            rainLock_.remainingHours -= static_cast<float>(elapsedMs) / 3600000.0f;
            if (rainLock_.remainingHours < 0.0f)
            {
                rainLock_.remainingHours = 0.0f;
            }
        }
    }

    rainLock_.active = rainLock_.remainingHours > 0.0f;
    rainLock_.lastUpdateMs = nowMs;

    const uint16_t hoursRemaining = rainLock_.remainingHours > 0.0f
                                        ? static_cast<uint16_t>(std::ceil(rainLock_.remainingHours))
                                        : 0;
    if (hoursRemaining != rainLock_.lastSentHours)
    {
        knx.getGroupObject(singleKoNumber(kKoRainLockRemaining)).value(hoursRemaining, DPT_TimePeriodHrs);
        rainLock_.lastSentHours = hoursRemaining;
    }

    if (rainLock_.active != rainLock_.lastSentActive)
    {
        knx.getGroupObject(singleKoNumber(kKoRainLockActive)).value(rainLock_.active, DPT_Switch);
        rainLock_.lastSentActive = rainLock_.active;
    }
}

bool SmartIrrigationModule::shouldProcessDecision(uint32_t nowMs) const
{
    return nowMs - lastDecisionMs_ >= 1000;
}

float SmartIrrigationModule::maxRainDelayFactor()
{
    const uint8_t zoneCount = std::min<uint8_t>(ParamSIR_SIR_ZoneCount, kMaxZones);
    float maxFactor = 0.0f;
    for (uint8_t zoneIndex = 0; zoneIndex < zoneCount; ++zoneIndex)
    {
        uint8_t _channelIndex = zoneIndex;
        (void)_channelIndex;
        const float factor = ParamSIR_SIR_ZRainDelayFactor;
        if (factor > maxFactor)
        {
            maxFactor = factor;
        }
    }
    return maxFactor;
}

SmartIrrigationModule openknxSmartIrrigationModule;
