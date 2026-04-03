#include "SmartIrrigationCore.h"

#include <cmath>

namespace SmartIrrigation
{
    namespace
    {
        // M-4: clampFloat defined in SmartIrrigationCore.h
    }

    SensorTimeoutTracker::SensorTimeoutTracker(uint8_t maxInvalid)
        : maxInvalid_(maxInvalid)
    {
    }

    bool SensorTimeoutTracker::update(bool valid)
    {
        if (valid)
        {
            reset();
            return false;
        }

        if (invalidCount_ < maxInvalid_)
        {
            ++invalidCount_;
        }

        if (invalidCount_ >= maxInvalid_)
        {
            failed_ = true;
        }

        return failed_;
    }

    void SensorTimeoutTracker::reset()
    {
        invalidCount_ = 0;
        failed_ = false;
    }

    bool SensorTimeoutTracker::failed() const
    {
        return failed_;
    }

    uint8_t SensorTimeoutTracker::invalidCount() const
    {
        return invalidCount_;
    }

    float calculateRainLockHours(float rainAmountMm, float factor, float remainingHours)
    {
        if (rainAmountMm <= 0.0f || factor <= 0.0f)
        {
            return remainingHours > 0.0f ? remainingHours : 0.0f;
        }

        float lockHours = rainAmountMm * factor;
        if (lockHours > 72.0f)
        {
            lockHours = 72.0f;
        }

        if (remainingHours > 0.0f)
        {
            return remainingHours > lockHours ? remainingHours : lockHours;
        }

        return lockHours;
    }

    float SprinklerConfig::calcPrecipRateMmh(float areaM2) const
    {
        if (count == 0) return 0.0f;

        switch (unit)
        {
            case FlowInputUnit::MmPerHour:
            {
                // Values are already in mm/h - average them
                float sum = 0.0f;
                for (uint8_t i = 0; i < count; i++)
                    sum += values[i];
                return sum / count;
            }
            case FlowInputUnit::LitersPerMinute:
            {
                if (areaM2 <= 0.0f) return 0.0f;
                // Sum l/min → convert to l/h → divide by area → mm/h
                float totalLpm = 0.0f;
                for (uint8_t i = 0; i < count; i++)
                    totalLpm += values[i];
                return (totalLpm * 60.0f) / areaM2;
            }
            case FlowInputUnit::CubicMetersPerHour:
            {
                if (areaM2 <= 0.0f) return 0.0f;
                // Sum m³/h → convert to l/h (*1000) → divide by area → mm/h
                float totalM3h = 0.0f;
                for (uint8_t i = 0; i < count; i++)
                    totalM3h += values[i];
                return (totalM3h * 1000.0f) / areaM2;
            }
            default:
                return 0.0f;
        }
    }

    float SprinklerConfig::calcTotalFlowLpm() const
    {
        float total = 0.0f;
        for (uint8_t i = 0; i < count; i++)
            total += values[i];

        switch (unit)
        {
            case FlowInputUnit::LitersPerMinute:
                return total;
            case FlowInputUnit::CubicMetersPerHour:
                return total * 1000.0f / 60.0f; // m³/h → l/min
            case FlowInputUnit::MmPerHour:
            default:
                return 0.0f; // Cannot convert mm/h to l/min without area
        }
    }

    float getEffectivePrecipRate(const ZoneSettings &zone)
    {
        if (zone.isDrip)
        {
            // Drip: l/h ÷ area = mm/h
            if (zone.areaM2 <= 0.0f || zone.dripFlowLph <= 0.0f) return 0.0f;
            return zone.dripFlowLph / zone.areaM2;
        }

        if (zone.isSmartPro)
        {
            // Smart Pro: compute from sprinkler config
            return zone.sprinklerConfig.calcPrecipRateMmh(zone.areaM2);
        }

        // Smart Core: direct mm/h value
        return zone.precipRateMmh;
    }

    float calculateRuntimeMinutes(float precipRateMmh, float waterDemandMm, uint16_t maxRuntimeMinutes)
    {
        if (precipRateMmh <= 0.0f || waterDemandMm <= 0.0f)
        {
            return 0.0f;
        }

        float runtime = (waterDemandMm / precipRateMmh) * 60.0f;
        if (maxRuntimeMinutes > 0 && runtime > maxRuntimeMinutes)
        {
            runtime = static_cast<float>(maxRuntimeMinutes);
        }

        return runtime;
    }

    uint16_t resolveMaxBucketMm(const ZoneSettings &zone)
    {
        // SoilType presets (mm): 0=disabled, 1=Sand/12, 2=SandigerLehm/18, 3=Lehm/25, 4=TonigerLehm/30, 5=Ton/35, 6=custom
        switch (zone.soilType)
        {
            case 1: return 12;
            case 2: return 18;
            case 3: return 25;
            case 4: return 30;
            case 5: return 35;
            case 6: return zone.maxBucketMm;
            default: return 0; // disabled
        }
    }

    float applyMaxBucket(float waterDemandMm, uint16_t maxBucketMm)
    {
        if (maxBucketMm == 0 || waterDemandMm <= 0.0f) return waterDemandMm;
        const float cap = static_cast<float>(maxBucketMm);
        return waterDemandMm > cap ? cap : waterDemandMm;
    }

    float drainageRateMmh(uint16_t drainageRateRaw)
    {
        return static_cast<float>(drainageRateRaw) * 0.1f;
    }

    DecisionResult decideWatering(const ZoneSettings &zone,
                                 const ZoneRuntimeState &runtime,
                                 const DecisionContext &context,
                                 const WeatherInputs &weather)
    {
        DecisionResult result;
        result.action = DecisionAction::None;
        result.mode = DecisionMode::Fallback;

        if (!context.systemOn || !zone.enabled || context.emergencyStop)
        {
            return result;
        }

        // Soil override: critically dry soil bypasses rain-lock and time windows (now per-zone via context)
        const bool soilOverride = context.hasSoilMoisture &&
                                  context.soilMoisturePercent < zone.soilThresholdPercent;

        // Rain-Lock blocks automatic starts UNLESS soil moisture is critically low
        if (context.rainLockActive && !soilOverride)
        {
            return result;
        }

        if (soilOverride)
        {
            result.soilOverride = true;
        }

        // Temperature gate: freeze protection and base-temp check
        if (weather.real.hasTemperature)
        {
            if (weather.real.temperatureC < static_cast<float>(context.minTempC))
            {
                return result;
            }

            const float baseLimit = static_cast<float>(zone.baseTempC) - zone.baseTempHyst;
            if (weather.real.temperatureC <= baseLimit)
            {
                return result;
            }
        }

        float et0 = 0.0f;
        float effectiveRain = 0.0f;
        float waterDemand = 0.0f;

        const float etFactor = zone.etFactorPercent / 100.0f;
        const float interception = zone.interceptionPercent / 100.0f;

        // Irrigation interval: 0 = 7 days default (Bug #1/#11: scale limits + ET0 sum to actual interval)
        const uint8_t interval = zone.irrigationIntervalDays > 0 ? zone.irrigationIntervalDays : 7u;
        const float intervalScale = static_cast<float>(interval) / 7.0f;
        // Scale weekly water limits proportionally to the configured interval
        const float effectiveMinWeek    = static_cast<float>(zone.minWeek) * intervalScale;
        const float effectiveMaxWeek    = zone.maxWeek    > 0 ? static_cast<float>(zone.maxWeek)    * intervalScale : 0.0f;
        const float effectiveAbsMaxWeek = zone.absMaxWeek > 0 ? static_cast<float>(zone.absMaxWeek) * intervalScale : 0.0f;

        // Count available ET0 days
        uint8_t et0Days = 0;
        for (uint8_t d = 0; d < ForecastWeatherInputs::kForecastDays; ++d)
            if (weather.forecast.hasEt0Day[d]) ++et0Days;

        // Only plan for the interval length (capped at available forecast days)
        const uint8_t planDays = interval < ForecastWeatherInputs::kForecastDays
                                     ? interval
                                     : ForecastWeatherInputs::kForecastDays;

        if (et0Days == ForecastWeatherInputs::kForecastDays)
        {
            // ── Weekly Planning Path: all 7 ET0 days available ──
            result.mode = DecisionMode::Forecast;
            float et0WeekSum = 0.0f;
            float rainWeekSum = 0.0f;
            const float drainPerDay = drainageRateMmh(zone.drainageRateRaw) * 24.0f;
            for (uint8_t d = 0; d < planDays; ++d)
            {
                et0WeekSum += weather.forecast.et0DayMm[d];
                if (weather.forecast.hasRainDay[d])
                {
                    const float dayRain = weather.forecast.rainDayMm[d] * (1.0f - interception);
                    rainWeekSum += clampFloat(dayRain - drainPerDay, 0.0f, dayRain);
                }
            }

            // Heat bonus applied to ETc BEFORE subtracting rain (Bug #7)
            float weeklyETc = et0WeekSum * etFactor;
            if (weather.real.hasTemperature)
            {
                const float heatDelta = weather.real.temperatureC - zone.baseTempC;
                if (heatDelta > 0.0f)
                {
                    const float heatFactor = 1.0f + clampFloat(heatDelta / 20.0f, 0.0f, 0.3f);
                    weeklyETc *= heatFactor;
                }
            }
            const float weeklyRain = rainWeekSum;
            float weeklyNeed = weeklyETc - weeklyRain;

            if (weeklyNeed < 0.0f) weeklyNeed = 0.0f;
            if (effectiveMinWeek > 0.0f && weeklyNeed < effectiveMinWeek)
                weeklyNeed = effectiveMinWeek;

            float remainingNeed = weeklyNeed - runtime.weekAmount;
            if (remainingNeed <= 0.0f)
            {
                et0 = weather.forecast.et0DayMm[0];
                effectiveRain = rainWeekSum / static_cast<float>(planDays);
                result.et0 = et0;
                result.effectiveRain = effectiveRain;
                return result;
            }

            const uint8_t remainingCycles = (zone.maxCycles > 0 && runtime.cycles < zone.maxCycles)
                ? static_cast<uint8_t>(zone.maxCycles - runtime.cycles)
                : 1;
            waterDemand = remainingNeed / static_cast<float>(remainingCycles);

            const uint16_t maxBucketW = resolveMaxBucketMm(zone);
            waterDemand = applyMaxBucket(waterDemand, maxBucketW);
            if (waterDemand < 0.0f) waterDemand = 0.0f;

            et0 = weather.forecast.et0DayMm[0];
            effectiveRain = rainWeekSum / static_cast<float>(planDays);
        }
        else if (et0Days > 0)
        {
            // ── Daily Path: ET0 Heute (day 0) available ──
            result.mode = DecisionMode::Forecast;
            et0 = weather.forecast.et0DayMm[0];
            const float rainAmount = weather.forecast.hasRainDay[0] ? weather.forecast.rainDayMm[0] : 0.0f;
            // Heat bonus applied to ETc BEFORE subtracting rain (Bug #7)
            float etc = et0 * etFactor;
            if (weather.real.hasTemperature)
            {
                const float heatDelta = weather.real.temperatureC - zone.baseTempC;
                if (heatDelta > 0.0f)
                {
                    const float heatFactor = 1.0f + clampFloat(heatDelta / 20.0f, 0.0f, 0.3f);
                    etc *= heatFactor;
                }
            }
            {
                const float drainPerDayD = drainageRateMmh(zone.drainageRateRaw) * 24.0f;
                const float rainPostInterception = rainAmount * (1.0f - interception);
                effectiveRain = clampFloat(rainPostInterception - drainPerDayD, 0.0f, rainPostInterception);
            }
            waterDemand = etc - effectiveRain;

            const uint16_t maxBucketD = resolveMaxBucketMm(zone);
            waterDemand = applyMaxBucket(waterDemand, maxBucketD);

            if (waterDemand < 0.0f) waterDemand = 0.0f;
        }
        else
        {
            // ── Fallback: no ET0 → time-based, use minPerCycle as demand ──
            result.mode = DecisionMode::Fallback;
            result.fallbackActive = true;
            waterDemand = zone.minPerCycle > 0 ? static_cast<float>(zone.minPerCycle) : 5.0f;
        }

        result.et0 = et0;
        result.effectiveRain = effectiveRain;

        // SoilOverride: ensure minimum demand even if weather indicates no watering needed (Bug #2)
        if (soilOverride && waterDemand <= 0.0f)
        {
            waterDemand = zone.minPerCycle > 0 ? static_cast<float>(zone.minPerCycle) : 5.0f;
        }

        if (waterDemand <= 0.0f)
        {
            return result;
        }

        if (effectiveAbsMaxWeek > 0.0f)
        {
            const float weekRemaining = effectiveAbsMaxWeek - runtime.weekAmount;
            if (weekRemaining <= 0.0f)
            {
                return result;
            }
        }

        if (effectiveMaxWeek > 0.0f && runtime.weekAmount + waterDemand > effectiveMaxWeek)
        {
            waterDemand = effectiveMaxWeek - runtime.weekAmount;
        }

        if (soilOverride)
        {
            waterDemand = zone.minPerCycle > 0 ? static_cast<float>(zone.minPerCycle) : waterDemand;
            if (waterDemand <= 0.0f)
            {
                return result;
            }
        }

        if (waterDemand < zone.minPerCycle)
        {
            return result;
        }

        if (zone.maxCycles > 0 && runtime.cycles >= zone.maxCycles)
        {
            if (waterDemand > zone.minPerCycle)
            {
                result.errorCode = 6;
            }
            return result;
        }

        const TimeWindowCheck windowCheck = inTimeWindow(context.nowMinutes,
                                                         zone.window1,
                                                         zone.window2,
                                                         zone.windowType,
                                                         context.allowWhenNoWindow);
        result.timeWindowConflict = windowCheck.conflict;
        // BUG-FIX EC-1: Allow soilOverride to bypass time window restrictions
        if (!windowCheck.allowed && !soilOverride)
        {
            result.action = DecisionAction::WaitTimeWindow;
            return result;
        }

        if (soilOverride)
        {
            result.action = DecisionAction::Start;
            result.waterDemand = zone.minPerCycle > 0 ? static_cast<float>(zone.minPerCycle) : waterDemand;
            return result;
        }

        result.waterDemand = waterDemand;
        result.action = DecisionAction::Start;
        return result;
    }

    TimeWindowCheck inTimeWindow(uint16_t nowMinutes,
                                 const TimeWindow &window1,
                                 const TimeWindow &window2,
                                 TimeWindowType type,
                                 bool allowWhenInactive)
    {
        TimeWindowCheck result;
        const bool anyActive = window1.active || window2.active;
        if (!anyActive)
        {
            result.allowed = allowWhenInactive;
            return result;
        }

        auto isWindowValid = [](const TimeWindow &window) {
            return !window.active || window.endMinutes != window.startMinutes;
        };

        if (!isWindowValid(window1) || !isWindowValid(window2))
        {
            result.conflict = true;
            result.allowed = false;
            return result;
        }

        auto checkWindow = [&](const TimeWindow &window) {
            if (!window.active)
                return false;

            if (type == TimeWindowType::Fixed)
            {
                if (isFixedWindowMatch(nowMinutes, window.startMinutes))
                {
                    result.fixedMatch = true;
                    return true;
                }
                return false;
            }

            return isWindowActive(nowMinutes, window);
        };

        result.allowed = checkWindow(window1) || checkWindow(window2);
        return result;
    }
}
