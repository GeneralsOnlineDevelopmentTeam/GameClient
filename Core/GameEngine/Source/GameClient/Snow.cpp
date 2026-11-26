/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// Snow.cpp ////////////////////////////////////////////////////////////////////////////////
// Snow Rendering implementation
// Author: Mark Wilczynski, July 2003
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the Game
#include "GameClient/Snow.h"
#include "GameClient/View.h"
#include "GameClient/Display.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/TerrainLogic.h"
#include "Common/AudioEventRTS.h"
#include "Common/AudioHandleSpecialValues.h"


SnowManager *TheSnowManager=nullptr;

SnowManager::SnowManager()
{
	m_time = 0;
	m_velocity = 1;
	m_isVisible = TRUE;	//default to showing if it's enabled via INI.
}

void SnowManager::init()
{
	//starting heights of each particle
	//TODO: replace this lookup table with some kind of procedural method that takes x,y as input.
	m_startingHeights=NEW Real [ SNOW_NOISE_X * SNOW_NOISE_Y];
	m_time = 0;

	updateIniSettings();
}

void SnowManager::updateIniSettings()
{
	Real *dst=m_startingHeights;
	//initialize a table of random starting positions for each particle.
	Int boxDimensions = (Int)TheWeatherSetting->m_snowBoxDimensions;
	for (Int y=0; y<SNOW_NOISE_Y; y++)
	{
		for (Int x=0; x<SNOW_NOISE_X; x++)
		{
			*dst=(Real)(rand()%(boxDimensions));
			dst++;
		}
	}

	m_velocity = TheWeatherSetting->m_snowVelocity;
	m_frequencyScaleX = TheWeatherSetting->m_snowFrequencyScaleX;
	m_frequencyScaleY = TheWeatherSetting->m_snowFrequencyScaleY;
	m_amplitude	= TheWeatherSetting->m_snowAmplitude;
	m_pointSize = TheWeatherSetting->m_snowPointSize;
	m_quadSize	= TheWeatherSetting->m_snowQuadSize;
	m_boxDimensions	= TheWeatherSetting->m_snowBoxDimensions;
	m_emitterSpacing = 1.0f/TheWeatherSetting->m_snowBoxDensity;
	m_maxPointSize = TheWeatherSetting->m_snowMaxPointSize;
	m_minPointSize = TheWeatherSetting->m_snowMinPointSize;

	//Time for snow flake to make it from top to bottom of rendered cube around camera.
	m_fullTimePeriod = m_boxDimensions/m_velocity;
}

void SnowManager::setVisible(Bool showWeather)
{
	m_isVisible = showWeather;
}

void SnowManager::setDynamicWeatherVisible(Bool visible)
{
	m_useDynamicVisibility = TRUE;
	m_dynamicVisible = visible;
}

void SnowManager::stopDynamicRain()
{
	if (TheAudio)
	{
		TheAudio->removeAudioEvent(m_rainSound);
		TheAudio->removeAudioEvent(m_thunderSound);
	}
	m_rainSound = AHSV_Error;
	m_thunderSound = AHSV_Error;
	m_nextLightningFrame = 0;
	m_thunderFrame = 0;
}

void SnowManager::setDynamicRain(Int profile, UnsignedInt seed)
{
	stopDynamicRain();
	if (TheAudio)
	{
		// Dynamic weather takes over the stock rain sounds from any map scripts.
		TheAudio->removeAudioEvent(AsciiString("RainStereoLoop"));
		TheAudio->removeAudioEvent(AsciiString("ThunderAmbienceStereo"));
		TheAudio->removeAudioEvent(AsciiString("ThunderStrikesStereo"));
		if (profile == WeatherSetting::DYNAMIC_HEAVY_RAIN || profile == WeatherSetting::DYNAMIC_STORM)
		{
			AudioEventRTS rain("RainStereoLoop");
			m_rainSound = TheAudio->addAudioEvent(&rain);
		}
	}
	if (profile == WeatherSetting::DYNAMIC_STORM && TheGameLogic)
	{
		m_stormSeed = seed;
		m_nextLightningFrame = TheGameLogic->getFrame() + 2 * LOGICFRAMES_PER_SECOND;
	}
}

void SnowManager::update()
{
	if (m_nextLightningFrame == 0 || !TheGameLogic)
		return;

	const UnsignedInt frame = TheGameLogic->getFrame();
	if (m_thunderFrame != 0 && frame >= m_thunderFrame)
	{
		if (TheAudio)
		{
			TheAudio->removeAudioEvent(m_thunderSound);
			AudioEventRTS thunder("ThunderStrikesStereo");
			m_thunderSound = TheAudio->addAudioEvent(&thunder);
		}
		m_thunderFrame = 0;
	}
	if (frame < m_nextLightningFrame)
		return;

	if (TheDisplay && TheTerrainLogic)
	{
		Region3D extent;
		TheTerrainLogic->getExtent(&extent);
		const Coord3D position = {
			(extent.lo.x + extent.hi.x) * 0.5f,
			(extent.lo.y + extent.hi.y) * 0.5f,
			extent.hi.z + 200.0f
		};
		const RGBColor color = { 0.65f, 0.7f, 0.85f };
		const Real radius = max(extent.hi.x - extent.lo.x, extent.hi.y - extent.lo.y) + 500.0f;
		TheDisplay->createLightPulse(&position, &color, radius, 100.0f, 1, LOGICFRAMES_PER_SECOND / 4);
	}

	// Visual randomness stays separate from the simulation and is seeded by the shared weather roll.
	m_stormSeed = m_stormSeed * 1664525u + 1013904223u;
	m_thunderFrame = frame + LOGICFRAMES_PER_SECOND / 2 + m_stormSeed % LOGICFRAMES_PER_SECOND;
	m_nextLightningFrame = frame + (8 + (m_stormSeed >> 16) % 11) * LOGICFRAMES_PER_SECOND;
}

void SnowManager::reset()
{
	stopDynamicRain();
	m_isVisible = TRUE;	//default to showing if it's enabled via INI.
	m_useDynamicVisibility = FALSE;
}

SnowManager::~SnowManager()
{
	stopDynamicRain();
	delete [] m_startingHeights;
	m_startingHeights=nullptr;

	// TheSuperHackers @fix Mauller 13/04/2025 Delete the instance of the weather settings
	deleteInstance((WeatherSetting*)TheWeatherSetting.getNonOverloadedPointer());
	TheWeatherSetting=nullptr;
}

OVERRIDE<WeatherSetting> TheWeatherSetting = nullptr;

static const char *const dynamicWeatherProfiles[] = { "AUTO", "DRY", "SNOW", "RAIN", "HEAVY_RAIN", "STORM", nullptr };

// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
const FieldParse WeatherSetting::m_weatherSettingFieldParseTable[] =
{
	{ "DynamicWeatherProfile", INI::parseIndexList, dynamicWeatherProfiles, offsetof( WeatherSetting, m_dynamicWeatherProfile ) },
	{ "SnowTexture",							INI::parseAsciiString,nullptr,			offsetof( WeatherSetting, m_snowTexture ) },
	{ "SnowFrequencyScaleX",					INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowFrequencyScaleX ) },
	{ "SnowFrequencyScaleY",					INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowFrequencyScaleY ) },
	{ "SnowAmplitude",							INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowAmplitude ) },
	{ "SnowPointSize",							INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowPointSize ) },
	{ "SnowMaxPointSize",						INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowMaxPointSize ) },
	{ "SnowMinPointSize",						INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowMinPointSize ) },
	{ "SnowQuadSize",							INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowQuadSize ) },
	{ "SnowBoxDimensions",						INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowBoxDimensions ) },
	{ "SnowBoxDensity",							INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowBoxDensity ) },
	{ "SnowVelocity",							INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowVelocity ) },
	{ "SnowPointSprites",						INI::parseBool,nullptr,			offsetof( WeatherSetting, m_usePointSprites ) },
	{ "SnowEnabled",							INI::parseBool,nullptr,			offsetof( WeatherSetting, m_snowEnabled ) },
	{ nullptr, nullptr, nullptr, 0 },
};

//-------------------------------------------------------------------------------------------------
void INI::parseWeatherDefinition( INI *ini )
{
	if (TheWeatherSetting == nullptr) {
		TheWeatherSetting = newInstance(WeatherSetting);
	} else if (ini->getLoadType() == INI_LOAD_CREATE_OVERRIDES) {
		WeatherSetting* ws = (WeatherSetting*) (TheWeatherSetting.getNonOverloadedPointer());
		WeatherSetting* wsOverride = newInstance(WeatherSetting);
		*wsOverride = *ws;

		// Mark that it is an override.
		wsOverride->markAsOverride();

		ws->friend_getFinalOverride()->setNextOverride(wsOverride);
	} else {
		throw INI_INVALID_DATA;
	}

	WeatherSetting* weatherSet = (WeatherSetting*) (TheWeatherSetting.getNonOverloadedPointer());
	weatherSet = (WeatherSetting*) (weatherSet->friend_getFinalOverride());
	// parse the data
	ini->initFromINI( weatherSet, TheWeatherSetting->getFieldParse() );

	if (TheSnowManager)
		TheSnowManager->updateIniSettings();

	if (ini->getLoadType() == INI_LOAD_CREATE_OVERRIDES) {
		// Check to see if we overrode any textures.
		// If we did, then we need to replace them in the model.
	}
}
