/// @addtogroup tools Tools
/// @brief Tools to help with administrating or building an NWN server.
/// @{
/// @file nwnx_tools.nss
#include "nwnx"

const string NWNX_Tools = "NWNX_Tools"; ///< @private

/// @brief Generates the minimap images for each area and outputs them to a "minimaps" folder in the user directory.
/// @param sMinimapPath Path to the folder where you store all the tileset's minimap images.
/// @note The minimap path should contain both the stock minimap images and any custom content.
/// @param bOverwriteExisting FALSE if you do not wish to overwrite existing map images.
/// @param sImageType The extension to determine the image type.
/// @param nImageSize The size to make each square minimap. Default is 16 pixels.
/// @param bTagFilename If TRUE will use the tag of the area for the filename instead of the resref.
void NWNX_Tools_GenerateMiniMaps(string sMinimapPath, int bOverwriteExisting = TRUE, string sImageType = "png", int nImageSize = 16, int bTagFilename = FALSE);

/// @brief Generates a DOT file of areas and their connections to visualize through graphviz.
/// @details Once the dot file is produced, builders can create visualizations through the dot command.
/// For example: `dot -Tsvg ~/nwn/graphs/ModuleName.dot > ~/nwn/graphs/ModuleName.svg`
/// @param sMinimapPath Path to the folder where you store your area's map images to be used as node shapes.
/// Can be generated with NWNX_Tools_GenerateMiniMaps(). If left blank will just use ellipses.
/// @param sOutputFile Name of the dot file to be output, default is ModuleName.dot.
/// File is saved in the userdir/graphs directory.
/// @param sImageType The extension of the minimap images.
/// @param bTagFilename If TRUE will look for the minimap file by area tag, not resref.
/// @param nLabelType 0 for resref, 1 for tag, 2 for area name
void NWNX_Tools_GenerateAreaGraph(string sMinimapPath = "", string sOutputFile = "", string sImageType = "png", int bTagFilename = FALSE, int nLabelType = 0);


/// @}

void NWNX_Tools_GenerateMiniMaps(string sMinimapPath, int bOverwriteExisting = TRUE, string sImageType = "png", int nImageSize = 16, int bTagFilename = FALSE)
{
    string sFunc = "GenerateMiniMaps";

    NWNX_PushArgumentInt(NWNX_Tools, sFunc, bTagFilename);
    NWNX_PushArgumentInt(NWNX_Tools, sFunc, nImageSize);
    NWNX_PushArgumentString(NWNX_Tools, sFunc, sImageType);
    NWNX_PushArgumentInt(NWNX_Tools, sFunc, bOverwriteExisting);
    NWNX_PushArgumentString(NWNX_Tools, sFunc, sMinimapPath);
    NWNX_CallFunction(NWNX_Tools, sFunc);
}

void NWNX_Tools_GenerateAreaGraph(string sMinimapPath = "", string sOutputFile = "", string sImageType = "png", int bTagFilename = FALSE, int nLabelType = 0)
{
    string sFunc = "GenerateAreaGraph";

    NWNX_PushArgumentInt(NWNX_Tools, sFunc, nLabelType);
    NWNX_PushArgumentInt(NWNX_Tools, sFunc, bTagFilename);
    NWNX_PushArgumentString(NWNX_Tools, sFunc, sImageType);
    NWNX_PushArgumentString(NWNX_Tools, sFunc, sOutputFile);
    NWNX_PushArgumentString(NWNX_Tools, sFunc, sMinimapPath);
    NWNX_CallFunction(NWNX_Tools, sFunc);
}
