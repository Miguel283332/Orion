// nlohmann/json va incluido en third_party/ (licencia MIT): no hace falta descargar nada para compilar.
#pragma once
#include "json.hpp"

namespace orion {
using json = nlohmann::json;
}
