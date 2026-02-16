#include "ModuleVersionCheck.h"
#include "SmartIrrigationModule.h"
#include "versions.h"

#include <algorithm>
#include <cmath>
#include <ctime>

#include "OpenKNX/DateTime.h"

namespace
{
    constexpr uint16_t kKoSingleOffset = 995;
    constexpr uint8_t kKoSingleCount = 53;
    constexpr uint16_t kKoZoneBase = 1100;
    constexpr uint8_t kKoZoneBlockSize = 15;

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
        kKoSensorTemperature = 20,
        kKoSensorRain = 21,
        kKoSensorRainAmount = 22,
        kKoSensorHumidity = 23,
        kKoSensorWind = 24,
        kKoSensorWindDirection = 25,
        kKoSensorSoilMoisture = 26,
        kKoSensorStatus = 27,
        kKoForecastTempCurrent = 40,
        kKoForecastTemp48h = 41,
        kKoForecastTemp7d = 42,
        kKoForecastRainCurrent = 43,
        kKoForecastRain48h = 44,
        kKoForecastRain7d = 45,
        kKoForecastRainAmountCurrent = 46,
        kKoForecastRainAmount48h = 47,
        kKoForecastRainAmount7d = 48,
        kKoForecastHumidity = 49,
        kKoForecastWind = 50,
        kKoForecastWindDirection = 51,
        kKoForecastUvIndex = 52,
        kKoForecastStatus = 53,
        kKoDiagMode = 300,
        kKoDiagEt0 = 301,
        kKoDiagEffectiveRain = 302,
        kKoDiagWaterDemand = 303,
        kKoDiagDecisionMode = 304,
        kKoDiagSequenceStatus = 305,
        kKoDiagFallback = 306
    };

    enum ZoneKoOffset : uint8_t
    {
        kZoneKoOnOff = 0,
        kZoneKoStatus = 1,
        kZoneKoState = 2,
        kZoneKoValve = 3,
        kZoneKoRemaining = 4,
        kZoneKoWeekAmount = 5,
        kZoneKoWeekAmountPrev = 6,
        kZoneKoLastTime = 7,
        kZoneKoLastAmount = 8,
        kZoneKoNextTime = 9,
        kZoneKoNextAmount = 10,
        kZoneKoError = 11,
        kZoneKoErrorCode = 12,
        kZoneKoPause = 13,
        kZoneKoResetWeek = 14
    };

    enum ZoneState : uint8_t
    {
        kZoneStateInactive = 0,
        kZoneStateWaiting = 1,
        kZoneStateActive = 2,
        kZoneStatePaused = 3
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

    float clampFloat(float value, float minValue, float maxValue)
    {
        return std::max(minValue, std::min(value, maxValue));
    }

    bool isInWindow(uint16_t nowMinutes, const SmartIrrigation::TimeWindow &window)
    {
        if (!window.active)
        {
            return false;
        }

        if (window.endMinutes >= window.startMinutes)
        {
            return nowMinutes >= window.startMinutes && nowMinutes <= window.endMinutes;
        }

        return nowMinutes >= window.startMinutes || nowMinutes <= window.endMinutes;
    }

    uint16_t minutesUntilEnd(uint16_t nowMinutes, uint16_t endMinutes)
    {
        if (endMinutes >= nowMinutes)
        {
            return static_cast<uint16_t>(endMinutes - nowMinutes);
        }
        return static_cast<uint16_t>((1440 - nowMinutes) + endMinutes);
    }

    bool isFixStartTime(uint16_t nowMinutes, uint16_t startMinutes)
    {
        const int diff = std::abs(static_cast<int>(nowMinutes) - static_cast<int>(startMinutes));
        return diff <= 1;
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
            const float demandPercent = (result.waterDemand / settings.maxWeek) * 100.0f;
            priority += clampFloat(demandPercent, 0.0f, 100.0f);
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
                hoursSince = 168.0f;
            }

            const float timePercent = clampFloat((hoursSince / 168.0f) * 100.0f, 0.0f, 100.0f);
            priority += timePercent;
        }

        if (settings.windowType == SmartIrrigation::TimeWindowType::Flexible)
        {
            bool inWindow = false;
            uint16_t minutesToEnd = 0;

            if (isInWindow(nowMinutes, settings.window1))
            {
                inWindow = true;
                minutesToEnd = minutesUntilEnd(nowMinutes, settings.window1.endMinutes);
            }
            else if (isInWindow(nowMinutes, settings.window2))
            {
                inWindow = true;
                minutesToEnd = minutesUntilEnd(nowMinutes, settings.window2.endMinutes);
            }

            if (inWindow && minutesToEnd <= 60)
            {
                priority += static_cast<float>(60 - minutesToEnd) * (50.0f / 60.0f);
            }
        }
        else
        {
            const bool fixMatch = (settings.window1.active && isFixStartTime(nowMinutes, settings.window1.startMinutes)) ||
                                  (settings.window2.active && isFixStartTime(nowMinutes, settings.window2.startMinutes));
            if (fixMatch)
            {
                priority += 500.0f;
                return priority;
            }
            return 0.0f;
        }

        float cyclesPoints = 50.0f;
        if (settings.maxCycles > 0)
        {
            const float cyclesPercent = clampFloat(static_cast<float>(cycles) / settings.maxCycles, 0.0f, 1.0f);
            cyclesPoints = (1.0f - cyclesPercent) * 50.0f;
        }
        priority += cyclesPoints;

        if (settings.maxWeek > 0)
        {
            const float weekPercent = weekAmount / settings.maxWeek;
            if (weekPercent > 0.8f)
            {
                priority -= (weekPercent - 0.8f) * 100.0f;
            }
        }

        return priority;
    }
}


const std::string SmartIrrigationModule::name()
{
    return "SmartIrrigation";
}

const std::string SmartIrrigationModule::version()
{
    return MODULE_SmartIrrigation_Version;
}

void SmartIrrigationModule::init()
{
}

void SmartIrrigationModule::setup(bool configured)
{
    (void)configured;

    loadWeightsFromParams();
}

void SmartIrrigationModule::loop(bool configured)
{
    (void)configured;

    const uint32_t nowMs = millis();
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
    const bool manualActive = manualMode || manualProgram;

    bool timeValid = openknx.time.isValid();
    auto localTime = openknx.time.getLocalTime();
    const uint16_t nowMinutes = timeValid ? static_cast<uint16_t>(localTime.hour * 60 + localTime.minute) : 0;
    const uint8_t month = timeValid ? localTime.month : 1;
    const uint32_t nowSec = timeValid ? static_cast<uint32_t>(localTime.toTime_t())
                                      : static_cast<uint32_t>(millis() / 1000);

    const bool realSensorsEnabled = ParamSIR_SIR_SensorEnable;
    const bool forecastEnabled = ParamSIR_SIR_ForecastEnable;

    const uint8_t zoneCount = std::min<uint8_t>(ParamSIR_SIR_ZoneCount, kMaxZones);
    uint8_t activeZones = 0;

    SmartIrrigation::DecisionResult bestDiag{};
    uint8_t bestDiagZone = 0;
    float bestDemand = -1.0f;

    bool sensorOk = realSensorsEnabled;
    if (realSensorsEnabled)
    {
        sensorOk &= !sensorHealth_[static_cast<size_t>(SmartIrrigation::SensorId::Temperature)].failed();
        sensorOk &= !sensorHealth_[static_cast<size_t>(SmartIrrigation::SensorId::Rain)].failed();
        sensorOk &= !sensorHealth_[static_cast<size_t>(SmartIrrigation::SensorId::RainAmount)].failed();
    }
    updateSensorStatusKo(sensorOk);

    if (emergencyStop)
    {
        stopAllZones(nowSec, timeValid);
        knx.getGroupObject(singleKoNumber(kKoActiveZones)).value(0, DPT_Value_1_Ucount);
        knx.getGroupObject(singleKoNumber(kKoSystemStatus)).value(false, DPT_Switch);
        knx.getGroupObject(singleKoNumber(kKoSystemError)).value(!sensorOk, DPT_Switch);
        return;
    }

    updateActiveZoneCountdown(nowSec, timeValid);

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

    for (uint8_t zoneIndex = 0; zoneIndex < zoneCount; ++zoneIndex)
    {
        SmartIrrigation::ZoneSettings settings = loadZoneSettings(zoneIndex);

        const bool zoneOnOff = knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoOnOff)).value(DPT_Switch);
        const bool zonePause = knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoPause)).value(DPT_Switch);
        const bool manualRequest = manualActive && zoneOnOff;
        manualRequested[zoneIndex] = manualRequest;
        pauseRequested[zoneIndex] = zonePause;

        settings.enabled = manualActive ? (settings.enabled && !zonePause)
                                         : (settings.enabled && zoneOnOff && !zonePause);
        settingsCache[zoneIndex] = settings;

        ZoneRuntime &runtimeState = zoneRuntime_[zoneIndex];

        if (runtimeState.state == kZoneStateActive && runtimeState.manualRun && (!manualActive || !manualRequest))
        {
            stopZone(zoneIndex, settings, nowSec, timeValid);
        }

        if (runtimeState.state == kZoneStateActive && zonePause && (manualActive || runtimeState.manualRun))
        {
            runtimeState.pauseRemainingMinutes = runtimeState.remainingMinutes;
            runtimeState.pauseStartSec = nowSec;
            runtimeState.state = kZoneStatePaused;
            runtimeState.plannedRuntimeSec = 0;
            runtimeState.startTimeSec = 0;
            runtimeState.nextTimeSec = 0;
            knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoValve)).value(false, DPT_Switch);
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
        context.rainLockActive = rainLock_.active;
        context.nowMinutes = nowMinutes;
        context.allowWhenNoWindow = true;
        context.realSensorsEnabled = realSensorsEnabled && sensorOk;
        context.forecastEnabled = forecastEnabled;
        context.month = month;

        SmartIrrigation::ZoneRuntimeState runtime{};
        runtime.weekAmount = zoneRuntime_[zoneIndex].weekAmount;
        runtime.cycles = zoneRuntime_[zoneIndex].cycles;

        SmartIrrigation::DecisionResult result = SmartIrrigation::decideWatering(settings,
                                                                                 runtime,
                                                                                 context,
                                                                                 weatherCache_,
                                                                                 forecastWeights_,
                                                                                 mixWeights_);
        decisions[zoneIndex] = result;

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
                if (settings.absMaxWeek > 0 && runtimeState.weekAmount >= settings.absMaxWeek)
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
                    queue[queueCount++] = {zoneIndex, 20000.0f, true};
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
            if (result.action == SmartIrrigation::DecisionAction::Start)
            {
                runtimeState.state = kZoneStateWaiting;
                runtimeState.remainingMinutes = 0;
                runtimeState.plannedRuntimeSec = 0;
                runtimeState.startTimeSec = 0;
                runtimeState.nextTimeSec = 0;
                runtimeState.nextAmount = round2(result.waterDemand);

                const float priority = result.soilOverride
                                           ? 10000.0f
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
                const bool fixMatch = (settings.window1.active && isFixStartTime(nowMinutes, settings.window1.startMinutes)) ||
                                      (settings.window2.active && isFixStartTime(nowMinutes, settings.window2.startMinutes));
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

    if (timeValid)
    {
        applyWeeklyReset(zoneCount,
                         nowMinutes,
                         localTime.dayOfWeek,
                         localTime.year,
                         localTime.month,
                         localTime.day,
                         timeValid);
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

    updateDiagOutputs(bestDiag, bestDiagZone);
}

uint16_t SmartIrrigationModule::flashSize()
{
    return 2 + (kMaxZones * (sizeof(float) * 2 + sizeof(uint8_t)));
}

void SmartIrrigationModule::writeFlash()
{
    openknx.flash.writeByte(kFlashVersion);
    openknx.flash.writeByte(kMaxZones);

    for (const auto &zone : zoneRuntime_)
    {
        openknx.flash.writeFloat(zone.weekAmount);
        openknx.flash.writeFloat(zone.weekAmountPrev);
        openknx.flash.writeByte(zone.cycles);
    }
}

void SmartIrrigationModule::readFlash(const uint8_t *data, const uint16_t size)
{
    (void)data;
    if (size == 0)
    {
        return;
    }

    const uint16_t expectedSize = flashSize();
    if (size < expectedSize)
    {
        logDebugP("SmartIrrigation flash data too small (%u < %u)", size, expectedSize);
    }

    const uint8_t version = openknx.flash.readByte();
    if (version != kFlashVersion)
    {
        logDebugP("SmartIrrigation flash version mismatch (%u)", version);
        return;
    }

    (void)openknx.flash.readByte();

    const uint16_t zoneEntrySize = static_cast<uint16_t>(sizeof(float) * 2 + sizeof(uint8_t));
    const uint8_t availableZones = size > 2 ? static_cast<uint8_t>((size - 2) / zoneEntrySize) : 0;
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
        }
    }
}

void SmartIrrigationModule::loadWeightsFromParams()
{
    forecastWeights_.high = ParamSIR_SIR_WeightHigh;
    forecastWeights_.mid = ParamSIR_SIR_WeightMid;
    forecastWeights_.low = ParamSIR_SIR_WeightLow;

    mixWeights_.real = ParamSIR_SIR_WeightReal;
    mixWeights_.forecast = ParamSIR_SIR_WeightForecast;

    const bool forecastOk = SmartIrrigation::normalizeForecastWeights(forecastWeights_);
    const bool mixOk = SmartIrrigation::normalizeMixWeights(mixWeights_);
    if (!forecastOk || !mixOk)
    {
        logInfoP("SmartIrrigation weights normalized to defaults");
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
    if (koNumber < kKoSingleOffset || koNumber >= kKoSingleOffset + kKoSingleCount)
    {
        return;
    }

    switch (koNumber - kKoSingleOffset + 1)
    {
        case kKoSensorTemperature:
        {
            const float value = ko.value(DPT_Value_Temp);
            const bool valid = isValidRange(value, -40.0f, 60.0f);
            weatherCache_.real.hasTemperature = valid;
            if (valid)
            {
                weatherCache_.real.temperatureC = value;
            }
            sensorHealth_[static_cast<size_t>(SmartIrrigation::SensorId::Temperature)].update(valid);
            break;
        }
        case kKoSensorRain:
        {
            const bool value = ko.value(DPT_Switch);
            weatherCache_.real.hasRain = true;
            weatherCache_.real.rainActive = value;
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
            }
            sensorHealth_[static_cast<size_t>(SmartIrrigation::SensorId::RainAmount)].update(valid);
            if (valid && weatherCache_.real.rainActive)
            {
                updateRainLockFromEvent(value, maxRainDelayFactor());
            }
            break;
        }
        case kKoSensorHumidity:
        {
            const float value = ko.value(DPT_Value_Humidity);
            const bool valid = isValidRange(value, 0.0f, 100.0f);
            weatherCache_.real.hasHumidity = valid;
            if (valid)
            {
                weatherCache_.real.humidityPercent = value;
            }
            sensorHealth_[static_cast<size_t>(SmartIrrigation::SensorId::Humidity)].update(valid);
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
        case kKoSensorWindDirection:
        {
            const uint8_t value = ko.value(DPT_Angle);
            const bool valid = value <= 360;
            weatherCache_.real.hasWindDirection = valid;
            if (valid)
            {
                weatherCache_.real.windDirectionDeg = value;
            }
            sensorHealth_[static_cast<size_t>(SmartIrrigation::SensorId::WindDirection)].update(valid);
            break;
        }
        case kKoSensorSoilMoisture:
        {
            const float value = ko.value(DPT_Value_Humidity);
            const bool valid = isValidRange(value, 0.0f, 100.0f);
            weatherCache_.real.hasSoilMoisture = valid;
            if (valid)
            {
                weatherCache_.real.soilMoisturePercent = value;
            }
            sensorHealth_[static_cast<size_t>(SmartIrrigation::SensorId::SoilMoisture)].update(valid);
            break;
        }
        case kKoForecastTempCurrent:
        {
            const float value = ko.value(DPT_Value_Temp);
            const bool valid = isValidRange(value, -40.0f, 60.0f);
            weatherCache_.forecast.hasTempCurrent = valid;
            if (valid)
            {
                weatherCache_.forecast.tempCurrentC = value;
            }
            break;
        }
        case kKoForecastTemp48h:
        {
            const float value = ko.value(DPT_Value_Temp);
            const bool valid = isValidRange(value, -40.0f, 60.0f);
            weatherCache_.forecast.hasTemp48h = valid;
            if (valid)
            {
                weatherCache_.forecast.temp48hC = value;
            }
            break;
        }
        case kKoForecastTemp7d:
        {
            const float value = ko.value(DPT_Value_Temp);
            const bool valid = isValidRange(value, -40.0f, 60.0f);
            weatherCache_.forecast.hasTemp7d = valid;
            if (valid)
            {
                weatherCache_.forecast.temp7dC = value;
            }
            break;
        }
        case kKoForecastRainCurrent:
        {
            weatherCache_.forecast.hasRainCurrent = true;
            weatherCache_.forecast.rainCurrent = ko.value(DPT_Switch);
            break;
        }
        case kKoForecastRain48h:
        {
            weatherCache_.forecast.hasRain48h = true;
            weatherCache_.forecast.rain48h = ko.value(DPT_Switch);
            break;
        }
        case kKoForecastRain7d:
        {
            weatherCache_.forecast.hasRain7d = true;
            weatherCache_.forecast.rain7d = ko.value(DPT_Switch);
            break;
        }
        case kKoForecastRainAmountCurrent:
        {
            const float value = ko.value(DPT_Rain_Amount);
            const bool valid = isValidRange(value, 0.0f, 200.0f);
            weatherCache_.forecast.hasRainAmountCurrent = valid;
            if (valid)
            {
                weatherCache_.forecast.rainAmountCurrentMm = value;
            }
            break;
        }
        case kKoForecastRainAmount48h:
        {
            const float value = ko.value(DPT_Rain_Amount);
            const bool valid = isValidRange(value, 0.0f, 200.0f);
            weatherCache_.forecast.hasRainAmount48h = valid;
            if (valid)
            {
                weatherCache_.forecast.rainAmount48hMm = value;
            }
            break;
        }
        case kKoForecastRainAmount7d:
        {
            const float value = ko.value(DPT_Rain_Amount);
            const bool valid = isValidRange(value, 0.0f, 200.0f);
            weatherCache_.forecast.hasRainAmount7d = valid;
            if (valid)
            {
                weatherCache_.forecast.rainAmount7dMm = value;
            }
            break;
        }
        case kKoForecastHumidity:
        {
            const float value = ko.value(DPT_Value_Humidity);
            const bool valid = isValidRange(value, 0.0f, 100.0f);
            weatherCache_.forecast.hasHumidity = valid;
            if (valid)
            {
                weatherCache_.forecast.humidityPercent = value;
            }
            break;
        }
        case kKoForecastWind:
        {
            const float value = ko.value(DPT_Value_Wsp_kmh);
            const bool valid = isValidRange(value, 0.0f, 180.0f);
            weatherCache_.forecast.hasWind = valid;
            if (valid)
            {
                weatherCache_.forecast.windSpeedKmh = value;
            }
            break;
        }
        case kKoForecastWindDirection:
        {
            const uint8_t value = ko.value(DPT_Angle);
            const bool valid = value <= 360;
            weatherCache_.forecast.hasWindDirection = valid;
            if (valid)
            {
                weatherCache_.forecast.windDirectionDeg = value;
            }
            break;
        }
        case kKoForecastUvIndex:
        {
            const uint8_t value = ko.value(DPT_Value_1_Ucount);
            weatherCache_.forecast.hasUvIndex = true;
            weatherCache_.forecast.uvIndex = value;
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
    settings.enabled = ParamSIR_ZEnabled;
    settings.areaM2 = ParamSIR_ZArea;
    settings.flowLpm = ParamSIR_ZFlow;
    settings.etFactorPercent = ParamSIR_ZETFactor;
    settings.interceptionPercent = ParamSIR_ZInterception;
    settings.minWeek = ParamSIR_ZMinWeek;
    settings.maxWeek = ParamSIR_ZMaxWeek;
    settings.absMaxWeek = ParamSIR_ZAbsMax;
    settings.maxCycles = ParamSIR_ZMaxCycles;
    settings.minPerCycle = ParamSIR_ZMinPerCycle;
    settings.manualRuntimeMinutes = ParamSIR_SIR_ZManualRuntime;
    settings.baseTempC = ParamSIR_ZBaseTemp;
    settings.baseTempHyst = ParamSIR_ZBaseTempHyst;
    settings.soilThresholdPercent = ParamSIR_ZSoilThreshold;
    settings.rainDelayFactor = ParamSIR_ZRainDelayFactor;
    settings.maxRuntimeMinutes = ParamSIR_ZMaxRuntime;
    settings.window1.active = ParamSIR_ZTW1Active;
    settings.window1.startMinutes = ParamSIR_ZTW1Start;
    settings.window1.endMinutes = ParamSIR_ZTW1End;
    settings.window2.active = ParamSIR_ZTW2Active;
    settings.window2.startMinutes = ParamSIR_ZTW2Start;
    settings.window2.endMinutes = ParamSIR_ZTW2End;
    settings.windowType = ParamSIR_ZTimeType == 0
                              ? SmartIrrigation::TimeWindowType::Flexible
                              : SmartIrrigation::TimeWindowType::Fixed;

    return settings;
}

void SmartIrrigationModule::updateZoneOutputs(uint8_t zoneIndex,
                                              const SmartIrrigation::DecisionResult &result,
                                              const SmartIrrigation::ZoneSettings &settings,
                                              const ZoneRuntime &runtime,
                                              bool timeValid)
{
    (void)settings;

    const uint16_t koStatus = zoneKoNumber(zoneIndex, kZoneKoStatus);
    const uint16_t koState = zoneKoNumber(zoneIndex, kZoneKoState);
    const uint16_t koValve = zoneKoNumber(zoneIndex, kZoneKoValve);
    const uint16_t koRemaining = zoneKoNumber(zoneIndex, kZoneKoRemaining);
    const uint16_t koWeekAmount = zoneKoNumber(zoneIndex, kZoneKoWeekAmount);
    const uint16_t koWeekAmountPrev = zoneKoNumber(zoneIndex, kZoneKoWeekAmountPrev);
    const uint16_t koLastTime = zoneKoNumber(zoneIndex, kZoneKoLastTime);
    const uint16_t koLastAmount = zoneKoNumber(zoneIndex, kZoneKoLastAmount);
    const uint16_t koNextTime = zoneKoNumber(zoneIndex, kZoneKoNextTime);
    const uint16_t koNextAmount = zoneKoNumber(zoneIndex, kZoneKoNextAmount);
    const uint16_t koError = zoneKoNumber(zoneIndex, kZoneKoError);
    const uint16_t koErrorCode = zoneKoNumber(zoneIndex, kZoneKoErrorCode);

    const bool isActive = runtime.state == kZoneStateActive;
    knx.getGroupObject(koStatus).value(isActive, DPT_Switch);
    knx.getGroupObject(koState).value(runtime.state, DPT_Value_1_Ucount);
    knx.getGroupObject(koValve).value(isActive, DPT_Switch);

    knx.getGroupObject(koRemaining).value(minutesToHoursCeil(runtime.remainingMinutes), DPT_TimePeriodHrs);

    knx.getGroupObject(koWeekAmount).value(runtime.weekAmount, DPT_Rain_Amount);
    knx.getGroupObject(koWeekAmountPrev).value(runtime.weekAmountPrev, DPT_Rain_Amount);
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
    knx.getGroupObject(koError).value(errorCode != 0, DPT_Switch);
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

    float waterDemandM2 = waterDemand;
    float runtimeMinutes = 0.0f;
    if (manualRuntimeMinutes > 0)
    {
        runtimeMinutes = static_cast<float>(manualRuntimeMinutes);
        if (settings.areaM2 > 0.0f)
        {
            waterDemandM2 = (runtimeMinutes * settings.flowLpm) / settings.areaM2;
        }
    }
    else
    {
        runtimeMinutes = SmartIrrigation::calculateRuntimeMinutes(settings.areaM2,
                                                                 settings.flowLpm,
                                                                 waterDemandM2,
                                                                 settings.maxRuntimeMinutes);
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
            const float totalLiters = waterDemandM2 * settings.areaM2;
            runtimeMinutes = settings.flowLpm > 0.0f ? (totalLiters / settings.flowLpm) : 0.0f;
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
    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoStatus)).value(true, DPT_Switch);
    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoState)).value(kZoneStateActive, DPT_Value_1_Ucount);
    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoRemaining)).value(minutesToHoursCeil(runtime.remainingMinutes),
                                                                       DPT_TimePeriodHrs);
    if (runtime.nextTimeSec > 0)
    {
        writeDateTimeKo(zoneKoNumber(zoneIndex, kZoneKoNextTime), runtime.nextTimeSec, timeValid);
    }
    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoNextAmount)).value(runtime.nextAmount, DPT_Rain_Amount);

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
    if (settings.areaM2 > 0.0f)
    {
        const float totalLiters = elapsedMinutes * settings.flowLpm;
        actualAmountM2 = totalLiters / settings.areaM2;
    }
    actualAmountM2 = round2(actualAmountM2);

    runtime.weekAmount += actualAmountM2;
    runtime.cycles += 1;
    runtime.lastTimeSec = nowSec;
    runtime.lastAmount = actualAmountM2;
    runtime.state = kZoneStateInactive;
    runtime.errorActive = false;
    runtime.errorCode = 0;
    runtime.remainingMinutes = 0;
    runtime.plannedRuntimeSec = 0;
    runtime.nextTimeSec = 0;
    runtime.nextAmount = 0.0f;
    runtime.manualRun = false;
    runtime.pauseRemainingMinutes = 0;
    runtime.pauseStartSec = 0;

    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoValve)).value(false, DPT_Switch);
    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoStatus)).value(false, DPT_Switch);
    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoState)).value(kZoneStateInactive, DPT_Value_1_Ucount);
    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoRemaining)).value(0, DPT_TimePeriodHrs);
    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoWeekAmount)).value(runtime.weekAmount, DPT_Rain_Amount);
    knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoLastAmount)).value(runtime.lastAmount, DPT_Rain_Amount);
    if (runtime.lastTimeSec > 0)
    {
        writeDateTimeKo(zoneKoNumber(zoneIndex, kZoneKoLastTime), runtime.lastTimeSec, timeValid);
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

uint8_t SmartIrrigationModule::stopAllZones(uint32_t nowSec, bool timeValid)
{
    uint8_t stopped = 0;
    for (uint8_t zoneIndex = 0; zoneIndex < kMaxZones; ++zoneIndex)
    {
        SmartIrrigation::ZoneSettings settings = loadZoneSettings(zoneIndex);
        if (zoneRuntime_[zoneIndex].state == kZoneStateActive)
        {
            if (stopZone(zoneIndex, settings, nowSec, timeValid))
            {
                ++stopped;
            }
        }
    }

    logWarningP("Notaus: %u Zonen gestoppt", static_cast<unsigned>(stopped));
    return stopped;
}

void SmartIrrigationModule::updateActiveZoneCountdown(uint32_t nowSec, bool timeValid)
{
    (void)timeValid;
    for (uint8_t zoneIndex = 0; zoneIndex < kMaxZones; ++zoneIndex)
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

void SmartIrrigationModule::applyWeeklyReset(uint8_t zoneCount,
                                             uint16_t nowMinutes,
                                             uint8_t dayOfWeek,
                                             uint16_t year,
                                             uint8_t month,
                                             uint8_t day,
                                             bool timeValid)
{
    if (!timeValid)
    {
        return;
    }

    const uint16_t resetMinutes = static_cast<uint16_t>(knx.paramWord(SIR_SIR_ResetTime));
    const uint8_t weekStartParam = ParamSIR_SIR_WeekStart;
    const uint8_t targetWeekday = weekStartParam == 0 ? 1 : 0; // Monday=1, Sunday=0

    if (dayOfWeek != targetWeekday || nowMinutes != resetMinutes)
    {
        return;
    }

    if (lastResetYear_ == year && lastResetMonth_ == month && lastResetDay_ == day)
    {
        return;
    }

    for (uint8_t zoneIndex = 0; zoneIndex < zoneCount; ++zoneIndex)
    {
        ZoneRuntime &runtime = zoneRuntime_[zoneIndex];
        runtime.weekAmountPrev = runtime.weekAmount;
        runtime.weekAmount = 0.0f;
        runtime.cycles = 0;

        knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoWeekAmountPrev)).value(runtime.weekAmountPrev,
                                                                                DPT_Rain_Amount);
        knx.getGroupObject(zoneKoNumber(zoneIndex, kZoneKoWeekAmount)).value(runtime.weekAmount, DPT_Rain_Amount);
    }

    saveRuntimeToFlash(true);

    lastResetYear_ = year;
    lastResetMonth_ = month;
    lastResetDay_ = day;
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
    knx.getGroupObject(singleKoNumber(kKoDiagFallback)).value(result.fallbackActive, DPT_Switch);
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
        const uint32_t elapsedMs = nowMs - rainLock_.lastUpdateMs;
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
        const float factor = ParamSIR_ZRainDelayFactor;
        if (factor > maxFactor)
        {
            maxFactor = factor;
        }
    }
    return maxFactor;
}

SmartIrrigationModule openknxSmartIrrigationModule;
