#include "SecretsManager.h"
#include <ArduinoJson.h>
#include <esp_partition.h>
#include <esp_spi_flash.h>

DynamicJsonDocument SecretsManager::doc(2048);
bool SecretsManager::loaded = false;

bool SecretsManager::load() {
  const uint32_t secretsOffset = 0x490000;
  const size_t maxSize = 2048;
  uint8_t buffer[maxSize + 1];  // +1 for null terminator

  const esp_partition_t* partition = esp_partition_find_first(
    ESP_PARTITION_TYPE_DATA,
    ESP_PARTITION_SUBTYPE_ANY,
    NULL
  );

  if (!partition) {
    Serial.println("❌ Failed to find data partition.");
    return false;
  }

  esp_err_t err = spi_flash_read(secretsOffset, buffer, maxSize);
  if (err != ESP_OK) {
    Serial.printf("❌ Flash read failed: %s\n", esp_err_to_name(err));
    return false;
  }

  buffer[maxSize] = '\0';  // Ensure null-termination just in case

  DeserializationError error = deserializeJson(doc, buffer);
  if (error) {
    Serial.println("❌ Failed to parse secrets from flash");
    return false;
  }

  loaded = true;
  Serial.println("✅ Secrets loaded from raw flash!");
  return true;
}

String SecretsManager::get(String key) {
  if (!loaded) {
    Serial.println("⚠️ Secrets not loaded! Call SecretsManager::load() first.");
    return "";
  }

  return doc[key] | "";
}