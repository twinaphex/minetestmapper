#include <cstdio>
#include <cstdlib>
#include <climits>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cerrno>
#include <cstring>
#include <vector>
#include "config.h"
#include "PlayerAttributes.h"
#include "TileGenerator.h"
#include "ZlibDecompressor.h"
#include "util.h"
#include "db-sqlite3.h"
#if USE_LEVELDB
#include "db-leveldb.h"
#endif
#if USE_REDIS
#include "db-redis.h"
#endif

using namespace std;

#define GETPIXEL(x, y) (getpixel(m_image, (x), (y)))
#define PUTPIXEL(x, y, c) (putpixel(m_image, (x), (y), (c)))

static inline uint16_t readU16(const unsigned char *data)
{
	return data[0] << 8 | data[1];
}

static inline unsigned int rgb2int(uint8_t r, uint8_t g, uint8_t b, uint8_t a=0xFF)
{
	return (a << 24) + (r << 16) + (g << 8) + b;
}

static inline unsigned int color2int(Color c)
{
    return rgb2int(c.r, c.g, c.b, c.a);
}

static inline unsigned int getpixel(FIBITMAP *i, unsigned int x, unsigned int y)
{
	RGBQUAD c;
	FreeImage_GetPixelColor(i, x, y, &c);
	return rgb2int(c.rgbRed, c.rgbGreen, c.rgbBlue);
}

static inline void putpixel(FIBITMAP *i, unsigned int x, unsigned int y, unsigned int c)
{
	RGBQUAD col;
	col.rgbRed = c & 0xff;
	col.rgbGreen = (c >> 8) & 0xff;
	col.rgbBlue = (c >> 16) & 0xff;
	FreeImage_SetPixelColor(i, x, y, &col);
}

// rounds n (away from 0) to a multiple of f while preserving the sign of n
static inline int round_multiple_nosign(int n, int f)
{
	int abs_n, sign;
	abs_n = (n >= 0) ? n : -n;
	sign = (n >= 0) ? 1 : -1;
	if (abs_n % f == 0)
		return n; // n == abs_n * sign
	else
		return sign * (abs_n + f - (abs_n % f));
}

static inline int readBlockContent(const unsigned char *mapData, int version, int datapos)
{
	if (version >= 24) {
		size_t index = datapos << 1;
		return (mapData[index] << 8) | mapData[index + 1];
	}
	else if (version >= 20) {
		if (mapData[datapos] <= 0x80) {
			return mapData[datapos];
		}
		else {
			return (int(mapData[datapos]) << 4) | (int(mapData[datapos + 0x2000]) >> 4);
		}
	}
	else {
		std::ostringstream oss;
		oss << "Unsupported map version " << version;
		throw std::runtime_error(oss.str());
	}
}

static inline int colorSafeBounds(int color)
{
	if (color > 255) {
		return 255;
	}
	else if (color < 0) {
		return 0;
	}
	else {
		return color;
	}
}

static inline Color mixColors(Color a, Color b)
{
    Color result;
    double a1 = a.a / 255.0;
    double a2 = b.a / 255.0;

    result.r = (int) (a1 * a.r + a2 * (1 - a1) * b.r);
    result.g = (int) (a1 * a.g + a2 * (1 - a1) * b.g);
    result.b = (int) (a1 * a.b + a2 * (1 - a1) * b.b);
    result.a = (int) (255 * (a1 + a2 * (1 - a1)));
    return result;
}

TileGenerator::TileGenerator():
	m_bgColor(255, 255, 255),
	m_scaleColor(0, 0, 0),
	m_originColor(255, 0, 0),
	m_playerColor(255, 0, 0),
	m_drawOrigin(false),
	m_drawPlayers(false),
	m_drawScale(false),
	m_drawAlpha(false),
	m_shading(true),
	m_backend(""),
	m_border(0),
	m_image(NULL),
	m_xMin(INT_MAX),
	m_xMax(INT_MIN),
	m_zMin(INT_MAX),
	m_zMax(INT_MIN),
	m_yMin(-30000),
	m_yMax(30000),
	m_geomX(-2048),
	m_geomY(-2048),
	m_geomX2(2048),
	m_geomY2(2048)
{
	FreeImage_Initialise();
	std::cout << "Using FreeImage v" << FreeImage_GetVersion() << std::endl;
}

TileGenerator::~TileGenerator()
{
	FreeImage_DeInitialise();
}

void TileGenerator::setBgColor(const std::string &bgColor)
{
	m_bgColor = parseColor(bgColor);
}

void TileGenerator::setScaleColor(const std::string &scaleColor)
{
	m_scaleColor = parseColor(scaleColor);
}

void TileGenerator::setOriginColor(const std::string &originColor)
{
	m_originColor = parseColor(originColor);
}

void TileGenerator::setPlayerColor(const std::string &playerColor)
{
	m_playerColor = parseColor(playerColor);
}

Color TileGenerator::parseColor(const std::string &color)
{
	Color parsed;
	if (color.length() != 7) {
		throw std::runtime_error("Color not 7 characters long");
	}
	if (color[0] != '#') {
		throw std::runtime_error("Color does not begin with #");
	}
	long col = strtol(color.c_str() + 1, NULL, 16);
	parsed.b = col % 256;
	col = col / 256;
	parsed.g = col % 256;
	col = col / 256;
	parsed.r = col % 256;
	return parsed;
}

void TileGenerator::setDrawOrigin(bool drawOrigin)
{
	m_drawOrigin = drawOrigin;
}

void TileGenerator::setDrawPlayers(bool drawPlayers)
{
	m_drawPlayers = drawPlayers;
}

void TileGenerator::setDrawScale(bool drawScale)
{
	m_drawScale = drawScale;
	if (m_drawScale) {
		m_border = 40;
	}
}

void TileGenerator::setDrawAlpha(bool drawAlpha)
{
    m_drawAlpha = drawAlpha;
}

void TileGenerator::setShading(bool shading)
{
	m_shading = shading;
}

void TileGenerator::setBackend(std::string backend)
{
	m_backend = backend;
}

void TileGenerator::setGeometry(int x, int y, int w, int h)
{
	m_geomX  = round_multiple_nosign(x, 16) / 16;
	m_geomY  = round_multiple_nosign(y, 16) / 16;
	m_geomX2 = round_multiple_nosign(x + w, 16) / 16;
	m_geomY2 = round_multiple_nosign(y + h, 16) / 16;
}

void TileGenerator::setMinY(int y)
{
	m_yMin = y;
}

void TileGenerator::setMaxY(int y)
{
	m_yMax = y;
}

void TileGenerator::parseColorsFile(const std::string &fileName)
{
	ifstream in;
	in.open(fileName.c_str(), ifstream::in);
	if (!in.is_open()) {
		return;
	}
	parseColorsStream(in);
}

void TileGenerator::generate(const std::string &input, const std::string &output)
{
	string input_path = input;
	if (input_path[input.length() - 1] != PATH_SEPARATOR) {
		input_path += PATH_SEPARATOR;
	}

	openDb(input_path);
	loadBlocks();
	createImage();
	renderMap();
	if (m_drawScale) {
		renderScale();
	}
	if (m_drawOrigin) {
		renderOrigin();
	}
	if (m_drawPlayers) {
		renderPlayers(input_path);
	}
	writeImage(output);
	printUnknown();
}

void TileGenerator::parseColorsStream(std::istream &in)
{
	char line[128], *p;
	while (in.good()) {
		in.getline(line, 128);
		p = line;
		while(*p++ != '\0')
			if(*p == '#') {
				*p = '\0'; // Cut off at the first #
				break;
			}

		char name[75];
		uint8_t r, g, b, a, t;
		a = 255;
		t = 0;

		sscanf(line, "%75s %hhu %hhu %hhu %hhu %hhu", name, &r, &g, &b, &a, &t);
		if(strlen(name) == 0)
			break;
		ColorEntry color = ColorEntry(r, g, b, a, t);
		m_colors[name] = color;
	}
}

void TileGenerator::openDb(const std::string &input)
{
	std::string backend = m_backend;
	if(backend == "") {
		std::ifstream ifs((input + "/world.mt").c_str());
		if(!ifs.good())
			throw std::runtime_error("Failed to read world.mt");
		backend = get_setting("backend", ifs);
		ifs.close();
	}

	if(backend == "sqlite3")
		m_db = new DBSQLite3(input);
#if USE_LEVELDB
	else if(backend == "leveldb")
		m_db = new DBLevelDB(input);
#endif
#if USE_REDIS
	else if(backend == "redis")
		m_db = new DBRedis(input);
#endif
	else
		throw std::runtime_error(((std::string) "Unknown map backend: ") + backend);
}

void TileGenerator::loadBlocks()
{
	std::vector<BlockPos> vec = m_db->getBlockPos();
	for (std::vector<BlockPos>::iterator it = vec.begin(); it != vec.end(); ++it) {
		BlockPos pos = *it;
		// Check that it's in geometry (from --geometry option)
		if (pos.x < m_geomX || pos.x > m_geomX2 || pos.z < m_geomY || pos.z > m_geomY2) {
			continue;
		}
		// Check that it's between --miny and --maxy
		if (pos.y < m_yMin * 16 || pos.y > m_yMax * 16) {
			continue;
		}
		// Adjust minimum and maximum positions to the nearest block
		if (pos.x < m_xMin) {
			m_xMin = pos.x;
		}
		if (pos.x > m_xMax) {
			m_xMax = pos.x;
		}
		if (pos.z < m_zMin) {
			m_zMin = pos.z;
		}
		if (pos.z > m_zMax) {
			m_zMax = pos.z;
		}
		m_positions.push_back(std::pair<int, int>(pos.x, pos.z));
	}
	m_positions.sort();
	m_positions.unique();
}

void TileGenerator::createImage()
{
	m_mapWidth = (m_xMax - m_xMin + 1) * 16;
	m_mapHeight = (m_zMax - m_zMin + 1) * 16;
	m_image = FreeImage_Allocate(m_mapWidth + m_border, m_mapHeight + m_border, 24);
	m_blockPixelAttributes.setWidth(m_mapWidth);
	// Background
	unsigned int c = color2int(m_bgColor);
	FreeImage_FillBackground(m_image, &c);
}

void TileGenerator::renderMap()
{
	std::list<int> zlist = getZValueList();
	for (std::list<int>::iterator zPosition = zlist.begin(); zPosition != zlist.end(); ++zPosition) {
		int zPos = *zPosition;
		std::map<int16_t, BlockList> blocks;
		m_db->getBlocksOnZ(blocks, zPos);
		for (std::list<std::pair<int, int> >::const_iterator position = m_positions.begin(); position != m_positions.end(); ++position) {
			if (position->second != zPos) {
				continue;
			}

			for (int i = 0; i < 16; ++i) {
				m_readedPixels[i] = 0;
				m_readInfo[i] = 0;
			}
			for (int i = 0; i < 16; i++) {
				for (int j = 0; j < 16; j++) {
					m_col[i][j] = m_bgColor; // This will be drawn by renderMapBlockBottom() for y-rows with only 'air', 'ignore' or unknown nodes if --drawalpha is used
					m_col[i][j].a = 0; // ..but set alpha to 0 to tell renderMapBlock() not to use this color to mix a shade
					m_th[i][j] = 0;
				}
			}

			int xPos = position->first;
			blocks[xPos].sort();
			const BlockList &blockStack = blocks[xPos];
			for (BlockList::const_iterator it = blockStack.begin(); it != blockStack.end(); ++it) {
				const BlockPos &pos = it->first;
				const unsigned char *data = it->second.c_str();
				size_t length = it->second.length();

				uint8_t version = data[0];
				//uint8_t flags = data[1];

				size_t dataOffset = 0;
				if (version >= 22) {
					dataOffset = 4;
				}
				else {
					dataOffset = 2;
				}

				ZlibDecompressor decompressor(data, length);
				decompressor.setSeekPos(dataOffset);
				ustring mapData = decompressor.decompress();
				ustring mapMetadata = decompressor.decompress();
				dataOffset = decompressor.seekPos();

				// Skip unused data
				if (version <= 21) {
					dataOffset += 2;
				}
				if (version == 23) {
					dataOffset += 1;
				}
				if (version == 24) {
					uint8_t ver = data[dataOffset++];
					if (ver == 1) {
						uint16_t num = readU16(data + dataOffset);
						dataOffset += 2;
						dataOffset += 10 * num;
					}
				}

				// Skip unused static objects
				dataOffset++; // Skip static object version
				int staticObjectCount = readU16(data + dataOffset);
				dataOffset += 2;
				for (int i = 0; i < staticObjectCount; ++i) {
					dataOffset += 13;
					uint16_t dataSize = readU16(data + dataOffset);
					dataOffset += dataSize + 2;
				}
				dataOffset += 4; // Skip timestamp

				m_blockAirId = -1;
				m_blockIgnoreId = -1;
				// Read mapping
				if (version >= 22) {
					dataOffset++; // mapping version
					uint16_t numMappings = readU16(data + dataOffset);
					dataOffset += 2;
					for (int i = 0; i < numMappings; ++i) {
						uint16_t nodeId = readU16(data + dataOffset);
						dataOffset += 2;
						uint16_t nameLen = readU16(data + dataOffset);
						dataOffset += 2;
						string name = string(reinterpret_cast<const char *>(data) + dataOffset, nameLen);
						if (name == "air") {
							m_blockAirId = nodeId;
						}
						else if (name == "ignore") {
							m_blockIgnoreId = nodeId;
						}
						else {
							m_nameMap[nodeId] = name;
						}
						dataOffset += nameLen;
					}
				}

				// Node timers
				if (version >= 25) {
					dataOffset++;
					uint16_t numTimers = readU16(data + dataOffset);
					dataOffset += 2;
					dataOffset += numTimers * 10;
				}

				renderMapBlock(mapData, pos, version);

				bool allReaded = true;
				for (int i = 0; i < 16; ++i) {
					if (m_readedPixels[i] != 0xffff) {
						allReaded = false;
					}
				}
				if (allReaded) {
					break;
				}
			}
			bool allReaded = true;
			for (int i = 0; i < 16; ++i) {
				if (m_readedPixels[i] != 0xffff) {
					allReaded = false;
				}
			}
			if (!allReaded) {
				renderMapBlockBottom(blockStack.begin()->first);
			}
		}
		if(m_shading)
			renderShading(zPos);
	}
}

inline void TileGenerator::renderMapBlock(const ustring &mapBlock, const BlockPos &pos, int version)
{
	int xBegin = (pos.x - m_xMin) * 16;
	int zBegin = (m_zMax - pos.z) * 16;
	const unsigned char *mapData = mapBlock.c_str();
	int minY = (pos.y * 16 > m_yMin) ? 0 : m_yMin - pos.y * 16;
	int maxY = (pos.y * 16 < m_yMax) ? 15 : m_yMax - pos.y * 16;
	for (int z = 0; z < 16; ++z) {
		int imageY = getImageY(zBegin + 15 - z);
		for (int x = 0; x < 16; ++x) {
			if (m_readedPixels[z] & (1 << x)) {
				continue;
			}
			int imageX = getImageX(xBegin + x);

			for (int y = maxY; y >= minY; --y) {
				int position = x + (y << 4) + (z << 8);
				int content = readBlockContent(mapData, version, position);
				if (content == m_blockIgnoreId || content == m_blockAirId) {
					continue;
				}
				NameMap::iterator blockName = m_nameMap.find(content);
				if (blockName == m_nameMap.end())
					continue;
				const string &name = blockName->second;
				ColorMap::const_iterator color = m_colors.find(name);
				if (color != m_colors.end()) {
					const Color c = color->second.to_color();
					if (m_drawAlpha) {
						if (m_col[z][x].a == 0)
							m_col[z][x] = c;
						else
							m_col[z][x] = mixColors(m_col[z][x], c);
						if(m_col[z][x].a == 0xFF) {
							PUTPIXEL(imageX, imageY, color2int(m_col[z][x]));
							m_readedPixels[z] |= (1 << x);
							m_blockPixelAttributes.attribute(15 - z, xBegin + x).thicken = m_th[z][x];
						} else {
							m_th[z][x] = (m_th[z][x] + color->second.t) / 2.0;
							continue;
						}
					} else {
						PUTPIXEL(imageX, imageY, color2int(c));
						m_readedPixels[z] |= (1 << x);
					}
					if(!(m_readInfo[z] & (1 << x))) {
						m_blockPixelAttributes.attribute(15 - z, xBegin + x).height = pos.y * 16 + y;
						m_readInfo[z] |= (1 << x);
					}
				} else {
					m_unknownNodes.insert(name);
					continue;
				}
				break;
			}
		}
	}
}

inline void TileGenerator::renderMapBlockBottom(const BlockPos &pos)
{
	int xBegin = (pos.x - m_xMin) * 16;
	int zBegin = (m_zMax - pos.z) * 16;
	for (int z = 0; z < 16; ++z) {
		int imageY = getImageY(zBegin + 15 - z);
		for (int x = 0; x < 16; ++x) {
			if (m_readedPixels[z] & (1 << x)) {
				continue;
			}
			int imageX = getImageX(xBegin + x);

			if (m_drawAlpha) {
				PUTPIXEL(imageX, imageY, color2int(m_col[z][x]));
				m_readedPixels[z] |= (1 << x);
				m_blockPixelAttributes.attribute(15 - z, xBegin + x).thicken = m_th[z][x];
			}
		}
	}
}

inline void TileGenerator::renderShading(int zPos)
{
	int zBegin = (m_zMax - zPos) * 16;
	for (int z = 0; z < 16; ++z) {
		int imageY = zBegin + z;
		if (imageY >= m_mapHeight) {
			continue;
		}
		imageY = getImageY(imageY);
		for (int x = 0; x < m_mapWidth; ++x) {
			if (!m_blockPixelAttributes.attribute(z, x).valid_height() || !m_blockPixelAttributes.attribute(z, x - 1).valid_height() || !m_blockPixelAttributes.attribute(z - 1, x).valid_height()) {
				continue;
			}
			int y = m_blockPixelAttributes.attribute(z, x).height;
			int y1 = m_blockPixelAttributes.attribute(z, x - 1).height;
			int y2 = m_blockPixelAttributes.attribute(z - 1, x).height;
			int d = ((y - y1) + (y - y2)) * 12;
			if (d > 36) {
				d = 36;
			}
			if (m_drawAlpha)
				d = d * ((0xFF - m_blockPixelAttributes.attribute(z, x).thicken) / 255.0);
			unsigned int sourceColor = GETPIXEL(getImageX(x), imageY);
			uint8_t r = (sourceColor & 0xff0000) >> 16;
			uint8_t g = (sourceColor & 0x00ff00) >> 8;
			uint8_t b = (sourceColor & 0x0000ff);
			r = colorSafeBounds(r + d);
			g = colorSafeBounds(g + d);
			b = colorSafeBounds(b + d);

			PUTPIXEL(getImageX(x), imageY, rgb2int(r, g, b));
		}
	}
	m_blockPixelAttributes.scroll();
}

void TileGenerator::renderScale()
{
	/*
	int color = color2int(m_scaleColor);
	gdImageString(m_image, gdFontGetMediumBold(), 24, 0, reinterpret_cast<unsigned char *>(const_cast<char *>("X")), color);
	gdImageString(m_image, gdFontGetMediumBold(), 2, 24, reinterpret_cast<unsigned char *>(const_cast<char *>("Z")), color);

	string scaleText;

	for (int i = (m_xMin / 4) * 4; i <= m_xMax; i += 4) {
		stringstream buf;
		buf << i * 16;
		scaleText = buf.str();

		int xPos = m_xMin * -16 + i * 16 + m_border;
		gdImageString(m_image, gdFontGetMediumBold(), xPos + 2, 0, reinterpret_cast<unsigned char *>(const_cast<char *>(scaleText.c_str())), color);
		gdImageLine(m_image, xPos, 0, xPos, m_border - 1, color);
	}

	for (int i = (m_zMax / 4) * 4; i >= m_zMin; i -= 4) {
		stringstream buf;
		buf << i * 16;
		scaleText = buf.str();

		int yPos = m_mapHeight - 1 - (i * 16 - m_zMin * 16) + m_border;
		gdImageString(m_image, gdFontGetMediumBold(), 2, yPos, reinterpret_cast<unsigned char *>(const_cast<char *>(scaleText.c_str())), color);
		gdImageLine(m_image, 0, yPos, m_border - 1, yPos, color);
	}
	*/
}

void TileGenerator::renderOrigin()
{
	/*
	int imageX = -m_xMin * 16 + m_border;
	int imageY = m_mapHeight - m_zMin * -16 + m_border;
	gdImageArc(m_image, imageX, imageY, 12, 12, 0, 360, color2int(m_originColor));
	*/
}

void TileGenerator::renderPlayers(const std::string &inputPath)
{
	/*
	int color = color2int(m_playerColor);

	PlayerAttributes players(inputPath);
	for (PlayerAttributes::Players::iterator player = players.begin(); player != players.end(); ++player) {
		int imageX = player->x / 10 - m_xMin * 16 + m_border;
		int imageY = m_mapHeight - (player->z / 10 - m_zMin * 16) + m_border;

		gdImageArc(m_image, imageX, imageY, 5, 5, 0, 360, color);
		gdImageString(m_image, gdFontGetMediumBold(), imageX + 2, imageY + 2, reinterpret_cast<unsigned char *>(const_cast<char *>(player->name.c_str())), color);
	}
	*/
}

inline std::list<int> TileGenerator::getZValueList() const
{
	std::list<int> zlist;
	for (std::list<std::pair<int, int> >::const_iterator position = m_positions.begin(); position != m_positions.end(); ++position) {
		zlist.push_back(position->second);
	}
	zlist.sort();
	zlist.unique();
	zlist.reverse();
	return zlist;
}

void TileGenerator::writeImage(const std::string &output)
{
	FreeImage_Save(FIF_PNG, m_image, output.c_str());
	FreeImage_Unload(m_image);
}

void TileGenerator::printUnknown()
{
	if (m_unknownNodes.size() > 0) {
		std::cerr << "Unknown nodes:" << std::endl;
		for (NameSet::iterator node = m_unknownNodes.begin(); node != m_unknownNodes.end(); ++node) {
			std::cerr << *node << std::endl;
		}
	}
}

inline int TileGenerator::getImageX(int val) const
{
	return val + m_border;
}

inline int TileGenerator::getImageY(int val) const
{
	return val + m_border;
}
