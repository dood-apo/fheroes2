/***************************************************************************
 *   fheroes2: https://github.com/ihhub/fheroes2                           *
 *   Copyright (C) 2019 - 2024                                             *
 *                                                                         *
 *   Free Heroes2 Engine: http://sourceforge.net/projects/fheroes2         *
 *   Copyright (C) 2009 by Andrey Afletdinov <fheroes2@gmail.com>          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "maps_fileinfo.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <list>
#include <locale>
#include <map>
#include <ostream>
#include <type_traits>
#include <utility>

#include "color.h"
#include "difficulty.h"
#include "dir.h"
#include "game_over.h"
#include "logging.h"
#include "map_format_info.h"
#include "maps_tiles.h"
#include "maps_tiles_helper.h"
#include "mp2.h"
#include "mp2_helper.h"
#include "race.h"
#include "serialize.h"
#include "settings.h"
#include "system.h"
#include "tools.h"

namespace
{
    const size_t mapNameLength = 16;
    const size_t mapDescriptionLength = 200;

    template <typename CharType>
    bool CaseInsensitiveCompare( const std::basic_string<CharType> & lhs, const std::basic_string<CharType> & rhs )
    {
        typename std::basic_string<CharType>::const_iterator li = lhs.begin();
        typename std::basic_string<CharType>::const_iterator ri = rhs.begin();

        while ( li != lhs.end() && ri != rhs.end() ) {
            const CharType lc = std::tolower( *li, std::locale() );
            const CharType rc = std::tolower( *ri, std::locale() );

            ++li;
            ++ri;

            if ( lc < rc ) {
                return true;
            }
            if ( lc > rc ) {
                return false;
            }
            // the chars are "equal", so proceed to check the next pair
        }

        // we came to the end of either (or both) strings, left is "smaller" if it was shorter:
        return li == lhs.end() && ri != rhs.end();
    }

    // This function returns an unsorted array. It is a caller responsibility to take care of sorting if needed.
    MapsFileInfoList getValidMaps( const ListFiles & mapFiles, const bool isMultiplayer, const bool isForEditor, const bool isOriginalMapFormat )
    {
        // create a list of unique maps (based on the map file name) and filter it by the preferred number of players
        std::map<std::string, Maps::FileInfo> uniqueMaps;

        const Settings & conf = Settings::Get();
        const int prefNumOfPlayers = conf.PreferablyCountPlayers();

        for ( const std::string & mapFile : mapFiles ) {
            Maps::FileInfo fi;

            if ( isOriginalMapFormat ) {
                if ( !fi.readMP2Map( mapFile, isForEditor ) ) {
                    continue;
                }
            }
            else {
                if ( !fi.readResurrectionMap( mapFile, isForEditor ) ) {
                    continue;
                }
            }

            if ( !isForEditor ) {
                if ( isMultiplayer ) {
                    assert( prefNumOfPlayers > 1 );
                    if ( !fi.isAllowCountPlayers( prefNumOfPlayers ) ) {
                        continue;
                    }
                }
                else {
                    const int humanOnlyColorsCount = Color::Count( fi.HumanOnlyColors() );

                    // Map has more than one human-only color, it is not suitable for single player mode
                    if ( humanOnlyColorsCount > 1 ) {
                        continue;
                    }

                    // Map has the human-only color, only this color can be selected by a human player
                    if ( humanOnlyColorsCount == 1 ) {
                        fi.removeHumanColors( fi.AllowCompHumanColors() );
                    }
                }
            }

            uniqueMaps.try_emplace( System::GetBasename( mapFile ), std::move( fi ) );
        }

        MapsFileInfoList result;
        result.reserve( uniqueMaps.size() );

        for ( auto & [name, info] : uniqueMaps ) {
            result.emplace_back( std::move( info ) );
        }

        return result;
    }
}

namespace Editor
{
    enum
    {
        Wins = 0x1000,
        CaptureTown = 0x1001,
        DefeatHero = 0x1002,
        FindArtifact = 0x1003,
        SideWins = 0x1004,
        AccumulateGold = 0x1005,
        CompAlsoWins = 0x0100,
        AllowNormalVictory = 0x0200,
        Loss = 0x2000,
        LoseTown = 0x2001,
        LoseHero = 0x2002,
        OutTime = 0x2003
    };
}

void Maps::FileInfo::Reset()
{
    filename.clear();
    name.clear();
    description.clear();

    width = 0;
    height = 0;
    difficulty = Difficulty::NORMAL;

    static_assert( std::is_same_v<decltype( races ), std::array<uint8_t, KINGDOMMAX>>, "Type of races has been changed, check the logic below" );
    static_assert( std::is_same_v<decltype( unions ), std::array<uint8_t, KINGDOMMAX>>, "Type of unions has been changed, check the logic below" );

    for ( int i = 0; i < KINGDOMMAX; ++i ) {
        races[i] = Race::NONE;
        unions[i] = Color::IndexToColor( i );
    }

    kingdomColors = 0;
    colorsAvailableForHumans = 0;
    colorsAvailableForComp = 0;
    colorsOfRandomRaces = 0;

    victoryConditionType = VICTORY_DEFEAT_EVERYONE;
    compAlsoWins = false;
    allowNormalVictory = false;
    victoryConditionParams.fill( 0 );

    lossConditionType = LOSS_EVERYTHING;
    lossConditionParams.fill( 0 );

    timestamp = 0;

    startWithHeroInEachCastle = false;

    version = GameVersion::SUCCESSION_WARS;

    worldDay = 0;
    worldWeek = 0;
    worldMonth = 0;
}

bool Maps::FileInfo::readMP2Map( std::string filePath, const bool isForEditor )
{
    Reset();

    StreamFile fs;
    if ( !fs.open( filePath, "rb" ) ) {
        DEBUG_LOG( DBG_GAME, DBG_WARN, "Error opening the file " << filePath )
        return false;
    }

    // magic byte
    if ( fs.getBE32() != 0x5C000000 ) {
        DEBUG_LOG( DBG_GAME, DBG_WARN, "File " << filePath << " is not a valid map file" )
        return false;
    }

    filename = std::move( filePath );

    // Difficulty level
    switch ( fs.getLE16() ) {
    case 0x00:
        difficulty = Difficulty::EASY;
        break;
    case 0x01:
        difficulty = Difficulty::NORMAL;
        break;
    case 0x02:
        difficulty = Difficulty::HARD;
        break;
    case 0x03:
        difficulty = Difficulty::EXPERT;
        break;
    default:
        difficulty = Difficulty::NORMAL;
        break;
    }

    // Width & height of the map in tiles
    width = fs.get();
    height = fs.get();

    const Colors colors( Color::ALL );

    // Colors used by kingdoms: blue, green, red, yellow, orange, purple
    for ( const int color : colors ) {
        if ( fs.get() != 0 ) {
            kingdomColors |= color;
        }
    }

    // Colors available for human players: blue, green, red, yellow, orange, purple
    for ( const int color : colors ) {
        if ( fs.get() != 0 ) {
            colorsAvailableForHumans |= color;
        }
    }

    // Colors available for computer players: blue, green, red, yellow, orange, purple
    for ( const int color : colors ) {
        if ( fs.get() != 0 ) {
            colorsAvailableForComp |= color;
        }
    }

    if ( !isForEditor && colorsAvailableForHumans == 0 ) {
        // This is not a valid map since no human players exist so it cannot be played.
        DEBUG_LOG( DBG_GAME, DBG_WARN, "Map " << filename << " does not contain any human players." )
        return false;
    }

    // TODO: Number of active kingdoms (unused)
    // fs.seekg(0x1A, std::ios_base::beg);
    // fs.get();

    fs.seek( 29 );
    // Victory condition type.
    victoryConditionType = fs.get();
    // Do the victory conditions apply to AI too?
    compAlsoWins = ( fs.get() != 0 );
    // Is "normal victory" (defeating all other players) applicable here?
    allowNormalVictory = ( fs.get() != 0 );
    // Parameter of victory condition.
    victoryConditionParams[0] = fs.getLE16();
    // Loss condition type.
    lossConditionType = fs.get();
    // Parameter of loss condition.
    lossConditionParams[0] = fs.getLE16();
    // Does the game start with heroes in castles automatically?
    startWithHeroInEachCastle = ( 0 == fs.get() );

    static_assert( std::is_same_v<decltype( races ), std::array<uint8_t, KINGDOMMAX>>, "Type of races has been changed, check the logic below" );

    // Initial races.
    for ( const int color : colors ) {
        const uint8_t race = Race::IndexToRace( fs.get() );
        const int idx = Color::GetIndex( color );
        assert( idx < KINGDOMMAX );

        races[idx] = race;

        if ( Race::RAND == race ) {
            colorsOfRandomRaces |= color;
        }
    }

    // Additional parameter of victory condition.
    victoryConditionParams[1] = fs.getLE16();
    // Additional parameter of loss condition.
    lossConditionParams[1] = fs.getLE16();

    bool skipUnionSetup = false;
    // If loss conditions are LOSS_HERO and victory conditions are VICTORY_DEFEAT_EVERYONE then we have to verify the color to which this object belongs to.
    // If the color is under computer control only we have to make it as an ally for human player.
    if ( lossConditionType == LOSS_HERO && victoryConditionType == VICTORY_DEFEAT_EVERYONE && Colors( colorsAvailableForHumans ).size() == 1 ) {
        // Each tile needs 16 + 8 + 8 + 8 + 8 + 8 + 8 + 8 + 8 + 16 + 32 + 32 = 160 bits or 20 bytes.
        fs.seek( MP2::MP2_MAP_INFO_SIZE + ( lossConditionParams[0] + lossConditionParams[1] * width ) * 20 );

        MP2::mp2tile_t mp2tile;
        MP2::loadTile( fs, mp2tile );

        Maps::Tiles tile;
        tile.Init( 0, mp2tile );

        std::pair<int, int> colorRace = getColorRaceFromHeroSprite( tile.GetObjectSpriteIndex() );
        if ( ( colorRace.first & colorsAvailableForHumans ) == 0 ) {
            const int side1 = colorRace.first | colorsAvailableForHumans;
            const int side2 = colorsAvailableForComp ^ colorRace.first;

            FillUnions( side1, side2 );

            victoryConditionType = VICTORY_DEFEAT_OTHER_SIDE;

            skipUnionSetup = true;
        }
    }

    // Map name
    fs.seek( 58 );
    name = fs.toString( mapNameLength );

    // Map description
    fs.seek( 118 );
    description = fs.toString( mapDescriptionLength );

    // Alliances of kingdoms
    if ( victoryConditionType == VICTORY_DEFEAT_OTHER_SIDE && !skipUnionSetup ) {
        int side1 = 0;
        int side2 = 0;

        const Colors availableColors( kingdomColors );
        if ( availableColors.empty() ) {
            DEBUG_LOG( DBG_GAME, DBG_WARN, "File " << filename << ": invalid list of kingdom colors during map load" )
            return false;
        }

        const int numPlayersSide1 = victoryConditionParams[0];
        if ( ( numPlayersSide1 <= 0 ) || ( numPlayersSide1 >= static_cast<int>( availableColors.size() ) ) ) {
            DEBUG_LOG( DBG_GAME, DBG_WARN, "File " << filename << ": invalid victory condition param during map load" )
            return false;
        }

        int playerIdx = 0;
        for ( const int color : availableColors ) {
            if ( playerIdx < numPlayersSide1 ) {
                side1 |= color;
            }
            else {
                side2 |= color;
            }

            ++playerIdx;
        }

        FillUnions( side1, side2 );
    }

    // Determine the type of the map
    const size_t pos = filename.rfind( '.' );
    if ( pos != std::string::npos ) {
        const std::string fileExtension = StringLower( filename.substr( pos + 1 ) );

        version = ( fileExtension == "mx2" || fileExtension == "hxc" ) ? GameVersion::PRICE_OF_LOYALTY : GameVersion::SUCCESSION_WARS;
    }

    return true;
}

bool Maps::FileInfo::readResurrectionMap( std::string filePath, const bool isForEditor )
{
    Reset();

    Maps::Map_Format::MapFormat map;
    if ( !Maps::Map_Format::loadBaseMap( filePath, map ) ) {
        return false;
    }

    filename = std::move( filePath );

    difficulty = map.difficulty;

    width = static_cast<uint16_t>( map.size );
    height = static_cast<uint16_t>( map.size );

    name = std::move( map.name );
    description = std::move( map.description );

    kingdomColors = map.availablePlayerColors;
    colorsAvailableForHumans = map.humanPlayerColors;
    colorsAvailableForComp = map.computerPlayerColors;

    races = map.playerRace;

    victoryConditionType = VICTORY_DEFEAT_EVERYONE;
    compAlsoWins = true;
    allowNormalVictory = true;

    lossConditionType = LOSS_EVERYTHING;

    for ( size_t i = 0; i < races.size(); ++i ) {
        if ( races[i] == Race::RAND ) {
            colorsOfRandomRaces |= static_cast<uint8_t>( 1 << i );
        }
    }

    version = GameVersion::RESURRECTION;

    if ( !isForEditor && colorsAvailableForHumans == 0 ) {
        // This is not a valid map since no human players exist so it cannot be played.
        DEBUG_LOG( DBG_GAME, DBG_WARN, "Map " << filename << " does not contain any human players." )
        return false;
    }

    return true;
}

void Maps::FileInfo::FillUnions( const int side1Colors, const int side2Colors )
{
    static_assert( std::is_same_v<decltype( unions ), std::array<uint8_t, KINGDOMMAX>>, "Type of unions has been changed, check the logic below" );

    using UnionsItemType = decltype( unions )::value_type;

    for ( int i = 0; i < KINGDOMMAX; ++i ) {
        const uint8_t color = Color::IndexToColor( i );

        if ( side1Colors & color ) {
            assert( side1Colors >= std::numeric_limits<UnionsItemType>::min() && side1Colors <= std::numeric_limits<UnionsItemType>::max() );

            unions[i] = static_cast<UnionsItemType>( side1Colors );
        }
        else if ( side2Colors & color ) {
            assert( side2Colors >= std::numeric_limits<UnionsItemType>::min() && side2Colors <= std::numeric_limits<UnionsItemType>::max() );

            unions[i] = static_cast<UnionsItemType>( side2Colors );
        }
        else {
            unions[i] = color;
        }
    }
}

bool Maps::FileInfo::sortByFileName( const FileInfo & lhs, const FileInfo & rhs )
{
    return CaseInsensitiveCompare( lhs.filename, rhs.filename );
}

bool Maps::FileInfo::sortByMapName( const FileInfo & lhs, const FileInfo & rhs )
{
    return CaseInsensitiveCompare( lhs.name, rhs.name );
}

int Maps::FileInfo::KingdomRace( int color ) const
{
    switch ( color ) {
    case Color::BLUE:
        return races[0];
    case Color::GREEN:
        return races[1];
    case Color::RED:
        return races[2];
    case Color::YELLOW:
        return races[3];
    case Color::ORANGE:
        return races[4];
    case Color::PURPLE:
        return races[5];
    default:
        break;
    }
    return 0;
}

uint32_t Maps::FileInfo::ConditionWins() const
{
    switch ( victoryConditionType ) {
    case VICTORY_DEFEAT_EVERYONE:
        return GameOver::WINS_ALL;
    case VICTORY_CAPTURE_TOWN:
        return allowNormalVictory ? ( GameOver::WINS_TOWN | GameOver::WINS_ALL ) : GameOver::WINS_TOWN;
    case VICTORY_KILL_HERO:
        return allowNormalVictory ? ( GameOver::WINS_HERO | GameOver::WINS_ALL ) : GameOver::WINS_HERO;
    case VICTORY_OBTAIN_ARTIFACT:
        return allowNormalVictory ? ( GameOver::WINS_ARTIFACT | GameOver::WINS_ALL ) : GameOver::WINS_ARTIFACT;
    case VICTORY_DEFEAT_OTHER_SIDE:
        return GameOver::WINS_SIDE;
    case VICTORY_COLLECT_ENOUGH_GOLD:
        return allowNormalVictory ? ( GameOver::WINS_GOLD | GameOver::WINS_ALL ) : GameOver::WINS_GOLD;
    default:
        // This is an unsupported winning condition! Please add the logic to handle it.
        assert( 0 );
        break;
    }

    return GameOver::COND_NONE;
}

uint32_t Maps::FileInfo::ConditionLoss() const
{
    switch ( lossConditionType ) {
    case LOSS_EVERYTHING:
        return GameOver::LOSS_ALL;
    case LOSS_TOWN:
        return GameOver::LOSS_TOWN;
    case LOSS_HERO:
        return GameOver::LOSS_HERO;
    case LOSS_OUT_OF_TIME:
        return GameOver::LOSS_TIME;
    default:
        // This is an unsupported loss condition! Please add the logic to handle it.
        assert( 0 );
        break;
    }

    return GameOver::COND_NONE;
}

bool Maps::FileInfo::WinsCompAlsoWins() const
{
    return compAlsoWins && ( ( GameOver::WINS_TOWN | GameOver::WINS_GOLD ) & ConditionWins() );
}

bool Maps::FileInfo::isAllowCountPlayers( int playerCount ) const
{
    const int humanOnly = Color::Count( HumanOnlyColors() );
    const int compHuman = Color::Count( AllowCompHumanColors() );

    return humanOnly <= playerCount && playerCount <= humanOnly + compHuman;
}

StreamBase & Maps::operator<<( StreamBase & msg, const FileInfo & fi )
{
    using VersionUnderlyingType = std::underlying_type_t<decltype( fi.version )>;

    // Only the basename of map filename (fi.file) is saved
    msg << System::GetBasename( fi.filename ) << fi.name << fi.description << fi.width << fi.height << fi.difficulty << static_cast<uint8_t>( KINGDOMMAX );

    static_assert( std::is_same_v<decltype( fi.races ), std::array<uint8_t, KINGDOMMAX>>, "Type of races has been changed, check the logic below" );
    static_assert( std::is_same_v<decltype( fi.unions ), std::array<uint8_t, KINGDOMMAX>>, "Type of unions has been changed, check the logic below" );

    for ( size_t i = 0; i < KINGDOMMAX; ++i ) {
        msg << fi.races[i] << fi.unions[i];
    }

    return msg << fi.kingdomColors << fi.colorsAvailableForHumans << fi.colorsAvailableForComp << fi.colorsOfRandomRaces << fi.victoryConditionType << fi.compAlsoWins
               << fi.allowNormalVictory << fi.victoryConditionParams[0] << fi.victoryConditionParams[1] << fi.lossConditionType << fi.lossConditionParams[0]
               << fi.lossConditionParams[1] << fi.timestamp << fi.startWithHeroInEachCastle << static_cast<VersionUnderlyingType>( fi.version ) << fi.worldDay
               << fi.worldWeek << fi.worldMonth;
}

StreamBase & Maps::operator>>( StreamBase & msg, FileInfo & fi )
{
    uint8_t kingdommax = 0;

    // Only the basename of map filename (fi.file) is loaded
    msg >> fi.filename >> fi.name >> fi.description >> fi.width >> fi.height >> fi.difficulty >> kingdommax;

    static_assert( std::is_same_v<decltype( fi.races ), std::array<uint8_t, KINGDOMMAX>>, "Type of races has been changed, check the logic below" );
    static_assert( std::is_same_v<decltype( fi.unions ), std::array<uint8_t, KINGDOMMAX>>, "Type of unions has been changed, check the logic below" );

    using RacesItemType = decltype( fi.races )::value_type;
    using UnionsItemType = decltype( fi.unions )::value_type;

    for ( size_t i = 0; i < kingdommax; ++i ) {
        RacesItemType racesItem = 0;
        UnionsItemType unionsItem = 0;

        msg >> racesItem >> unionsItem;

        if ( i < KINGDOMMAX ) {
            fi.races[i] = racesItem;
            fi.unions[i] = unionsItem;
        }
    }

    msg >> fi.kingdomColors >> fi.colorsAvailableForHumans >> fi.colorsAvailableForComp >> fi.colorsOfRandomRaces >> fi.victoryConditionType >> fi.compAlsoWins
        >> fi.allowNormalVictory >> fi.victoryConditionParams[0] >> fi.victoryConditionParams[1] >> fi.lossConditionType >> fi.lossConditionParams[0]
        >> fi.lossConditionParams[1] >> fi.timestamp >> fi.startWithHeroInEachCastle;

    using VersionUnderlyingType = std::underlying_type_t<decltype( fi.version )>;
    static_assert( std::is_same_v<VersionUnderlyingType, int>, "Type of version has been changed, check the logic below" );

    VersionUnderlyingType version = 0;
    msg >> version;

    fi.version = static_cast<GameVersion>( version );

    return msg >> fi.worldDay >> fi.worldWeek >> fi.worldMonth;
}

MapsFileInfoList Maps::getAllMapFileInfos( const bool isForEditor, const bool isMultiplayer )
{
    ListFiles maps = Settings::FindFiles( "maps", ".mp2", false );

    const bool isPOLSupported = Settings::Get().isPriceOfLoyaltySupported();

    if ( isPOLSupported ) {
        maps.Append( Settings::FindFiles( "maps", ".mx2", false ) );
    }

    MapsFileInfoList validMaps = getValidMaps( maps, isMultiplayer, isForEditor, true );

    if ( isPOLSupported ) {
        const ListFiles resurrectionMaps = Settings::FindFiles( "maps", ".fh2m", false );
        MapsFileInfoList validResurrectionMaps = getValidMaps( resurrectionMaps, isMultiplayer, isForEditor, false );

        validMaps.reserve( maps.size() + resurrectionMaps.size() );

        for ( auto & map : validResurrectionMaps ) {
            validMaps.emplace_back( std::move( map ) );
        }
    }

    std::sort( validMaps.begin(), validMaps.end(), Maps::FileInfo::sortByMapName );

    return validMaps;
}

MapsFileInfoList Maps::getResurrectionMapFileInfos( const bool isForEditor, const bool isMultiplayer )
{
    if ( !Settings::Get().isPriceOfLoyaltySupported() ) {
        // Resurrection maps require POL resources presence.
        return {};
    }

    const ListFiles maps = Settings::FindFiles( "maps", ".fh2m", false );
    MapsFileInfoList validMaps = getValidMaps( maps, isMultiplayer, isForEditor, false );

    std::sort( validMaps.begin(), validMaps.end(), Maps::FileInfo::sortByMapName );

    return validMaps;
}
