#pragma once

#include <cstdint>


namespace SmartIrrigation
{
    struct ForecastWeights
    {
        float high = 0.6f;
        float mid = 0.3f;
        float low = 0.1f;
    };

    struct MixWeights
    {
        float real = 0.7f;
        float forecast = 0.3f;
    };

    struct RealWeatherInputs
    {
        bool hasTemperature = false;
        float temperatureC = 0.0f;
        bool hasRain = false;
        bool rainActive = false;
        bool hasRainAmount = false;
        float rainAmountMm = 0.0f;
        bool hasHumidity = false;
        float humidityPercent = 0.0f;
        bool hasWind = false;
        float windSpeedMs = 0.0f;
        bool hasWindDirection = false;
        float windDirectionDeg = 0.0f;
        bool hasUvIndex = false;
        float uvIndex = 0.0f;
        bool hasSoilMoisture = false;
        float soilMoisturePercent = 0.0f;
    };

    struct ForecastWeatherInputs
    {
        bool hasTempCurrent = false;
        bool hasTemp48h = false;
        bool hasTemp7d = false;
        float tempCurrentC = 0.0f;
        float temp48hC = 0.0f;
        float temp7dC = 0.0f;

        bool hasRainCurrent = false;
        bool hasRain48h = false;
        bool hasRain7d = false;
        bool rainCurrent = false;
        bool rain48h = false;
        bool rain7d = false;

        bool hasRainAmountCurrent = false;
        bool hasRainAmount48h = false;
        bool hasRainAmount7d = false;
        float rainAmountCurrentMm = 0.0f;
        float rainAmount48hMm = 0.0f;
        float rainAmount7dMm = 0.0f;

        bool hasHumidity = false;
        float humidityPercent = 0.0f;
        bool hasWind = false;
        float windSpeedKmh = 0.0f;
        bool hasWindDirection = false;
        float windDirectionDeg = 0.0f;
        bool hasUvIndex = false;
        float uvIndex = 0.0f;
    };

    struct WeatherInputs
    {
        RealWeatherInputs real{};
        ForecastWeatherInputs forecast{};
    };

    struct TimeWindow
    {
        bool active = false;
        uint16_t startMinutes = 0;
        uint16_t endMinutes = 0;
    };

    enum class TimeWindowType : uint8_t
    {
        Flexible = 0,
        Fixed = 1
    };

    struct TimeWindowCheck
    {
        bool allowed = false;
        bool conflict = false;
        bool fixedMatch = false;
    };

    struct ZoneSettings
    {
        bool enabled = false;
        float areaM2 = 0.0f;
        float flowLpm = 0.0f;
        uint8_t etFactorPercent = 100;
        uint8_t interceptionPercent = 0;
        uint8_t minWeek = 0;
        uint8_t maxWeek = 0;
        uint8_t absMaxWeek = 0;
        uint8_t maxCycles = 0;
        uint8_t minPerCycle = 0;
        uint16_t manualRuntimeMinutes = 0;
        uint8_t baseTempC = 0;
        uint8_t baseTempHyst = 0;
        uint8_t soilThresholdPercent = 0;
        float rainDelayFactor = 0.0f;
        uint16_t maxRuntimeMinutes = 0;
        TimeWindow window1{};
        TimeWindow window2{};
        TimeWindowType windowType = TimeWindowType::Flexible;
    };

    struct ZoneRuntimeState
    {
        float weekAmount = 0.0f;
        uint8_t cycles = 0;
    };

    struct DecisionContext
    {
        bool systemOn = false;
        bool emergencyStop = false;
        bool rainLockActive = false;
        uint16_t nowMinutes = 0;
        bool allowWhenNoWindow = true;
        bool realSensorsEnabled = false;
        bool forecastEnabled = false;
        int8_t minTempC = 0;
        uint8_t month = 1;
    };

    enum class DecisionMode : uint8_t
    {
        Real = 1,
        Forecast = 2,
        Mixed = 3,
        Fallback = 4
    };

    enum class DecisionAction : uint8_t
    {
        None = 0,
        WaitTimeWindow = 1,
        Start = 2
    };

    struct DecisionResult
    {
        DecisionAction action = DecisionAction::None;
        DecisionMode mode = DecisionMode::Fallback;
        float waterDemand = 0.0f;
        float et0 = 0.0f;
        float effectiveRain = 0.0f;
        bool soilOverride = false;
        bool fallbackActive = false;
        uint8_t errorCode = 0;
        bool timeWindowConflict = false;
    };

    enum class SensorId : uint8_t
    {
        Temperature = 0,
        Rain,
        RainAmount,
        Humidity,
        Wind,
        WindDirection,
        SoilMoisture,
        Count
    };

    class SensorTimeoutTracker
    {
      public:
                SensorTimeoutTracker(uint8_t maxInvalid = 3);
        bool update(bool valid);
        void reset();
        bool failed() const;
        uint8_t invalidCount() const;

      private:
        uint8_t maxInvalid_ = 3;
        uint8_t invalidCount_ = 0;
        bool failed_ = false;
    };

    float fallbackEt0ForMonth(uint8_t month);

    bool normalizeForecastWeights(ForecastWeights &weights);
    bool normalizeMixWeights(MixWeights &weights);

    bool validateForecastWeights(const ForecastWeights &weights, float epsilon = 0.001f);
    bool validateMixWeights(const MixWeights &weights, float epsilon = 0.001f);

    float calculateRainLockHours(float rainAmountMm, float factor, float remainingHours);
    float calculateRuntimeMinutes(float areaM2, float flowLpm, float waterDemand, uint16_t maxRuntimeMinutes);
    float calculateEt0Real(const RealWeatherInputs &inputs);
    float calculateEt0Forecast(const ForecastWeatherInputs &inputs, const ForecastWeights &weights);
    DecisionResult decideWatering(const ZoneSettings &zone,
                                 const ZoneRuntimeState &runtime,
                                 const DecisionContext &context,
                                 const WeatherInputs &weather,
                                 const ForecastWeights &forecastWeights,
                                 const MixWeights &mixWeights);

    TimeWindowCheck inTimeWindow(uint16_t nowMinutes,
                                 const TimeWindow &window1,
                                 const TimeWindow &window2,
                                 TimeWindowType type,
                                 bool allowWhenInactive = true);
}
