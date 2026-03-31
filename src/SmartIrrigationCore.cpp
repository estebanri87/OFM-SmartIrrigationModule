#include "SmartIrrigationCore.h"

#include <cmath>

namespace SmartIrrigation
{
    namespace
    {
        constexpr float kFallbackEt0Table[12] = {
            0.5f, 1.0f, 2.0f, 3.5f, 4.5f, 5.5f, 6.0f, 5.5f, 4.0f, 2.5f, 1.0f, 0.5f
        };

        bool normalizeWeights(float &a, float &b, float &c, float defA, float defB, float defC)
        {
            const float sum = a + b + c;
            if (sum <= 0.0f)
            {
                a = defA;
                b = defB;
                c = defC;
                return false;
            }

            a /= sum;
            b /= sum;
            c /= sum;
            return true;
        }

        bool normalizeWeights(float &a, float &b, float defA, float defB)
        {
            const float sum = a + b;
            if (sum <= 0.0f)
            {
                a = defA;
                b = defB;
                return false;
            }

            a /= sum;
            b /= sum;
            return true;
        }

        bool isWithinRange(float value, float minValue, float maxValue)
        {
            return value >= minValue && value <= maxValue;
        }

        float clampFloat(float value, float minValue, float maxValue)
        {
            if (value < minValue)
            {
                return minValue;
            }
            if (value > maxValue)
            {
                return maxValue;
            }
            return value;
        }

        float weightedAverage3(float a, float b, float c, const ForecastWeights &weights)
        {
            return a * weights.high + b * weights.mid + c * weights.low;
        }

        // BUG-FIX: Validated weighted average that ignores invalid (missing) values
        float weightedAverageValidated3(float a, bool hasA, float b, bool hasB, float c, bool hasC, const ForecastWeights &weights)
        {
            float totalWeight = 0.0f;
            float weightedSum = 0.0f;

            if (hasA)
            {
                weightedSum += a * weights.high;
                totalWeight += weights.high;
            }
            if (hasB)
            {
                weightedSum += b * weights.mid;
                totalWeight += weights.mid;
            }
            if (hasC)
            {
                weightedSum += c * weights.low;
                totalWeight += weights.low;
            }

            return totalWeight > 0.0f ? (weightedSum / totalWeight) : 0.0f;
        }

        float windMsToKmh(float valueMs)
        {
            return valueMs * 3.6f;
        }

        bool hasRealMustSensors(const RealWeatherInputs &inputs)
        {
            return inputs.hasTemperature && inputs.hasRain;
        }

        bool hasForecastMustSensors(const ForecastWeatherInputs &inputs)
        {
            return inputs.hasTempCurrent && inputs.hasRainCurrent;
        }
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

    float fallbackEt0ForMonth(uint8_t month)
    {
        if (month < 1 || month > 12)
        {
            return kFallbackEt0Table[0];
        }

        return kFallbackEt0Table[month - 1];
    }

    bool normalizeForecastWeights(ForecastWeights &weights)
    {
        return normalizeWeights(weights.high, weights.mid, weights.low, 0.6f, 0.3f, 0.1f);
    }

    bool normalizeMixWeights(MixWeights &weights)
    {
        return normalizeWeights(weights.real, weights.forecast, 0.7f, 0.3f);
    }

    bool validateForecastWeights(const ForecastWeights &weights, float epsilon)
    {
        if (!isWithinRange(weights.high, 0.0f, 1.0f) || !isWithinRange(weights.mid, 0.0f, 1.0f) ||
            !isWithinRange(weights.low, 0.0f, 1.0f))
        {
            return false;
        }

        const float sum = weights.high + weights.mid + weights.low;
        return std::fabs(sum - 1.0f) <= epsilon;
    }

    bool validateMixWeights(const MixWeights &weights, float epsilon)
    {
        if (!isWithinRange(weights.real, 0.0f, 1.0f) || !isWithinRange(weights.forecast, 0.0f, 1.0f))
        {
            return false;
        }

        const float sum = weights.real + weights.forecast;
        return std::fabs(sum - 1.0f) <= epsilon;
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

    float calculateRuntimeMinutes(float areaM2, float flowLpm, float waterDemand, uint16_t maxRuntimeMinutes)
    {
        if (areaM2 <= 0.0f || flowLpm <= 0.0f || waterDemand <= 0.0f)
        {
            return 0.0f;
        }

        const float totalLiters = waterDemand * areaM2;
        float runtime = totalLiters / flowLpm;
        if (maxRuntimeMinutes > 0 && runtime > maxRuntimeMinutes)
        {
            runtime = static_cast<float>(maxRuntimeMinutes);
        }

        return runtime;
    }

    float calculateEt0Real(const RealWeatherInputs &inputs)
    {
        if (!inputs.hasTemperature)
        {
            return 0.0f;
        }

        const float tempC = inputs.temperatureC;
        const float humidity = inputs.hasHumidity ? inputs.humidityPercent : 50.0f;
        const float windKmh = inputs.hasWind ? windMsToKmh(inputs.windSpeedMs) : 5.0f;
        const float uv = inputs.hasUvIndex ? inputs.uvIndex : 5.0f;

        const float tempFactor = clampFloat((tempC + 5.0f) * 0.15f, 0.0f, 10.0f);
        const float humidityFactor = clampFloat((100.0f - humidity) * 0.02f, 0.0f, 2.0f);
        const float windFactor = clampFloat(windKmh * 0.02f, 0.0f, 2.5f);
        const float uvFactor = clampFloat(uv * 0.1f, 0.0f, 2.0f);

        return tempFactor + humidityFactor + windFactor + uvFactor;
    }

    float calculateEt0Forecast(const ForecastWeatherInputs &inputs, const ForecastWeights &weights)
    {
        if (!inputs.hasTempCurrent && !inputs.hasTemp48h && !inputs.hasTemp7d)
        {
            return 0.0f;
        }

        // BUG-FIX: Use validated weighted average to handle missing temp values correctly
        const float tempC = weightedAverageValidated3(
            inputs.tempCurrentC, inputs.hasTempCurrent,
            inputs.temp48hC, inputs.hasTemp48h,
            inputs.temp7dC, inputs.hasTemp7d,
            weights);
        const float humidity = inputs.hasHumidity ? inputs.humidityPercent : 50.0f;
        const float windKmh = inputs.hasWind ? inputs.windSpeedKmh : 5.0f;
        const float uv = inputs.hasUvIndex ? inputs.uvIndex : 5.0f;

        const float tempFactor = clampFloat((tempC + 5.0f) * 0.15f, 0.0f, 10.0f);
        const float humidityFactor = clampFloat((100.0f - humidity) * 0.02f, 0.0f, 2.0f);
        const float windFactor = clampFloat(windKmh * 0.02f, 0.0f, 2.5f);
        const float uvFactor = clampFloat(uv * 0.1f, 0.0f, 2.0f);

        return tempFactor + humidityFactor + windFactor + uvFactor;
    }

    DecisionResult decideWatering(const ZoneSettings &zone,
                                 const ZoneRuntimeState &runtime,
                                 const DecisionContext &context,
                                 const WeatherInputs &weather,
                                 const ForecastWeights &forecastWeights,
                                 const MixWeights &mixWeights)
    {
        DecisionResult result;
        result.action = DecisionAction::None;
        result.mode = DecisionMode::Fallback;

        if (!context.systemOn || !zone.enabled || context.emergencyStop)
        {
            return result;
        }

        // BUG-FIX EC-6: Check soilOverride BEFORE rainLock to allow critical soil moisture to override
        const bool soilOverride = weather.real.hasSoilMoisture &&
                                  weather.real.soilMoisturePercent < zone.soilThresholdPercent;

        // Rain-Lock blocks automatic starts UNLESS soil moisture is critically low
        if (context.rainLockActive && !soilOverride)
        {
            return result;
        }

        if (soilOverride)
        {
            result.soilOverride = true;
        }

        DecisionMode mode = DecisionMode::Fallback;
        if (context.realSensorsEnabled && context.forecastEnabled)
        {
            mode = DecisionMode::Mixed;
        }
        else if (context.realSensorsEnabled)
        {
            mode = DecisionMode::Real;
        }
        else if (context.forecastEnabled)
        {
            mode = DecisionMode::Forecast;
        }

        bool fallbackActive = false;
        if (mode == DecisionMode::Real)
        {
            if (!hasRealMustSensors(weather.real))
            {
                mode = DecisionMode::Fallback;
                fallbackActive = true;
            }
        }
        else if (mode == DecisionMode::Forecast)
        {
            if (!hasForecastMustSensors(weather.forecast))
            {
                mode = DecisionMode::Fallback;
                fallbackActive = true;
            }
        }
        else if (mode == DecisionMode::Mixed)
        {
            const bool realOk = hasRealMustSensors(weather.real);
            const bool forecastOk = hasForecastMustSensors(weather.forecast);
            if (!realOk && !forecastOk)
            {
                mode = DecisionMode::Fallback;
                fallbackActive = true;
            }
            else if (!realOk)
            {
                mode = DecisionMode::Forecast;
            }
            else if (!forecastOk)
            {
                mode = DecisionMode::Real;
            }
        }

        float temperatureC = 0.0f;
        bool hasTemperature = false;
        if (mode == DecisionMode::Real)
        {
            temperatureC = weather.real.temperatureC;
            hasTemperature = weather.real.hasTemperature;
        }
        else if (mode == DecisionMode::Forecast)
        {
            temperatureC = weather.forecast.tempCurrentC;
            hasTemperature = weather.forecast.hasTempCurrent;
        }
        else if (mode == DecisionMode::Mixed)
        {
            const float tempForecast = weather.forecast.tempCurrentC;
            temperatureC = weather.real.temperatureC * mixWeights.real + tempForecast * mixWeights.forecast;
            hasTemperature = weather.real.hasTemperature || weather.forecast.hasTempCurrent;
        }

        if (hasTemperature)
        {
            if (temperatureC < static_cast<float>(context.minTempC))
            {
                return result;
            }

            const float baseLimit = static_cast<float>(zone.baseTempC) - zone.baseTempHyst;
            if (temperatureC <= baseLimit)
            {
                return result;
            }
        }
        else if (mode != DecisionMode::Fallback)
        {
            return result;
        }
        else if (context.realSensorsEnabled || context.forecastEnabled)
        {
            return result;
        }

        float et0 = 0.0f;
        float effectiveRain = 0.0f;
        float waterDemand = 0.0f;

        if (mode == DecisionMode::Fallback)
        {
            et0 = fallbackEt0ForMonth(context.month);
        }
        else if (mode == DecisionMode::Real)
        {
            et0 = calculateEt0Real(weather.real);
        }
        else if (mode == DecisionMode::Forecast)
        {
            et0 = calculateEt0Forecast(weather.forecast, forecastWeights);
        }
        else
        {
            const float et0Real = calculateEt0Real(weather.real);
            const float et0Forecast = calculateEt0Forecast(weather.forecast, forecastWeights);
            et0 = et0Real * mixWeights.real + et0Forecast * mixWeights.forecast;
        }

        float rainAmount = 0.0f;
        if (mode == DecisionMode::Real)
        {
            if (weather.real.hasRainAmount)
            {
                rainAmount = weather.real.rainAmountMm;
            }
        }
        else if (mode == DecisionMode::Forecast)
        {
            // Rain-bool fallback: estimate 5mm when rain expected but no amount available
            constexpr float kRainBoolFallbackMm = 5.0f;
            const float currentMm = weather.forecast.hasRainAmountCurrent ? weather.forecast.rainAmountCurrentMm
                                  : (weather.forecast.hasRainCurrent && weather.forecast.rainCurrent ? kRainBoolFallbackMm : 0.0f);
            const float h48Mm = weather.forecast.hasRainAmount48h ? weather.forecast.rainAmount48hMm
                              : (weather.forecast.hasRain48h && weather.forecast.rain48h ? kRainBoolFallbackMm : 0.0f);
            const float d7Mm = weather.forecast.hasRainAmount7d ? weather.forecast.rainAmount7dMm
                             : (weather.forecast.hasRain7d && weather.forecast.rain7d ? kRainBoolFallbackMm : 0.0f);
            // Use validated weighted average with fallback values
            const bool hasCurrent = weather.forecast.hasRainAmountCurrent || (weather.forecast.hasRainCurrent && weather.forecast.rainCurrent);
            const bool has48h = weather.forecast.hasRainAmount48h || (weather.forecast.hasRain48h && weather.forecast.rain48h);
            const bool has7d = weather.forecast.hasRainAmount7d || (weather.forecast.hasRain7d && weather.forecast.rain7d);
            rainAmount = weightedAverageValidated3(currentMm, hasCurrent, h48Mm, has48h, d7Mm, has7d, forecastWeights);
        }
        else if (mode == DecisionMode::Mixed)
        {
            float realRain = 0.0f;
            if (weather.real.hasRainAmount)
            {
                realRain = weather.real.rainAmountMm;
            }
            // Rain-bool fallback for forecast portion in mixed mode
            constexpr float kRainBoolFallbackMm = 5.0f;
            const float currentMm = weather.forecast.hasRainAmountCurrent ? weather.forecast.rainAmountCurrentMm
                                  : (weather.forecast.hasRainCurrent && weather.forecast.rainCurrent ? kRainBoolFallbackMm : 0.0f);
            const float h48Mm = weather.forecast.hasRainAmount48h ? weather.forecast.rainAmount48hMm
                              : (weather.forecast.hasRain48h && weather.forecast.rain48h ? kRainBoolFallbackMm : 0.0f);
            const float d7Mm = weather.forecast.hasRainAmount7d ? weather.forecast.rainAmount7dMm
                             : (weather.forecast.hasRain7d && weather.forecast.rain7d ? kRainBoolFallbackMm : 0.0f);
            const bool hasCurrent = weather.forecast.hasRainAmountCurrent || (weather.forecast.hasRainCurrent && weather.forecast.rainCurrent);
            const bool has48h = weather.forecast.hasRainAmount48h || (weather.forecast.hasRain48h && weather.forecast.rain48h);
            const bool has7d = weather.forecast.hasRainAmount7d || (weather.forecast.hasRain7d && weather.forecast.rain7d);
            const float forecastRain = weightedAverageValidated3(currentMm, hasCurrent, h48Mm, has48h, d7Mm, has7d, forecastWeights);
            rainAmount = realRain * mixWeights.real + forecastRain * mixWeights.forecast;
        }

        const float etFactor = zone.etFactorPercent / 100.0f;
        const float interception = zone.interceptionPercent / 100.0f;
        const float etc = et0 * etFactor;
        effectiveRain = rainAmount * (1.0f - interception);
        waterDemand = etc - effectiveRain;

        if (hasTemperature)
        {
            const float heatDelta = temperatureC - zone.baseTempC;
            if (heatDelta > 0.0f)
            {
                const float heatFactor = 1.0f + clampFloat(heatDelta / 20.0f, 0.0f, 0.3f);
                waterDemand *= heatFactor;
            }
        }

        if (zone.maxWeek > 0)
        {
            waterDemand = clampFloat(waterDemand, 0.0f, static_cast<float>(zone.maxWeek));
        }
        else if (waterDemand < 0.0f)
        {
            waterDemand = 0.0f;
        }

        result.mode = mode;
        result.fallbackActive = fallbackActive;
        result.et0 = et0;
        result.effectiveRain = effectiveRain;

        if (waterDemand <= 0.0f)
        {
            return result;
        }

        if (zone.absMaxWeek > 0)
        {
            const float weekRemaining = static_cast<float>(zone.absMaxWeek) - runtime.weekAmount;
            if (weekRemaining <= 0.0f)
            {
                return result;
            }
        }

        if (zone.maxWeek > 0 && runtime.weekAmount + waterDemand > zone.maxWeek)
        {
            waterDemand = zone.maxWeek - runtime.weekAmount;
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
            result.mode = mode;
            result.fallbackActive = fallbackActive;
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
            {
                return false;
            }

            if (type == TimeWindowType::Fixed)
            {
                const int diff = std::abs(static_cast<int>(nowMinutes) - static_cast<int>(window.startMinutes));
                const int circularDiff = std::min(diff, 1440 - diff);
                if (circularDiff <= 1)
                {
                    result.fixedMatch = true;
                    return true;
                }
                return false;
            }

            if (window.endMinutes > window.startMinutes)
            {
                return nowMinutes >= window.startMinutes && nowMinutes <= window.endMinutes;
            }

            return nowMinutes >= window.startMinutes || nowMinutes <= window.endMinutes;
        };

        result.allowed = checkWindow(window1) || checkWindow(window2);
        return result;
    }
}
