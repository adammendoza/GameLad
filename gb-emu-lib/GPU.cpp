#include "pch.hpp"
#include "GPU.hpp"

/*
    FF40 - LCDC - LCD Control (R/W)
    Bit 7 - LCD Display Enable             (0=Off, 1=On)
    Bit 6 - Window Tile Map Display Select (0=9800-9BFF, 1=9C00-9FFF)
    Bit 5 - Window Display Enable          (0=Off, 1=On)
    Bit 4 - BG & Window Tile Data Select   (0=8800-97FF, 1=8000-8FFF)
    Bit 3 - BG Tile Map Display Select     (0=9800-9BFF, 1=9C00-9FFF)
    Bit 2 - OBJ (Sprite) Size              (0=8x8, 1=8x16)
    Bit 1 - OBJ (Sprite) Display Enable    (0=Off, 1=On)
    Bit 0 - BG Display (for CGB see below) (0=Off, 1=On)
*/
#define IsLCDDisplayEnabled ISBITSET(m_LCDControl, 7)
#define WindowTileMapDisplaySelect ISBITSET(m_LCDControl, 6)
#define WindowDisplayEnable ISBITSET(m_LCDControl, 5)
#define BGWindowTileDataSelect ISBITSET(m_LCDControl, 4)
#define BGWTileMapDisplaySelect ISBITSET(m_LCDControl, 3)
#define OBJSize ISBITSET(m_LCDControl, 2)
#define OBJDisplayEnable ISBITSET(m_LCDControl, 1)
#define BGDisplayEnable ISBITSET(m_LCDControl, 0)

/*
    FF41 - STAT - LCDC Status (R/W)
    Bit 6 - LYC=LY Coincidence Interrupt (1=Enable) (Read/Write)
    Bit 5 - Mode 2 OAM Interrupt         (1=Enable) (Read/Write)
    Bit 4 - Mode 1 V-Blank Interrupt     (1=Enable) (Read/Write)
    Bit 3 - Mode 0 H-Blank Interrupt     (1=Enable) (Read/Write)
    Bit 2 - Coincidence Flag  (0:LYC<>LY, 1:LYC=LY) (Read Only)
    Bit 1-0 - Mode Flag       (Mode 0-3, see below) (Read Only)
        0: During H-Blank
        1: During V-Blank
        2: During Searching OAM-RAM
        3: During Transfering Data to LCD Driver
*/
#define LYCoincidenceInterrupt ISBITSET(m_LCDControllerStatus , 6)
#define OAMInterrupt ISBITSET(m_LCDControllerStatus , 5)
#define VBlankInterrupt ISBITSET(m_LCDControllerStatus , 4)
#define HBlankInterrupt ISBITSET(m_LCDControllerStatus , 3)
#define SETMODE(mode) m_LCDControllerStatus = ((m_LCDControllerStatus & ~0x03) | mode)
#define GETMODE (m_LCDControllerStatus & 0x03)

const byte GBColors[]
{
    0xEB, 0xC4, 0x60, 0x00
};

GPU::GPU(IMMU* pMMU, ICPU* pCPU) :
    m_MMU(pMMU),
    m_CPU(pCPU),
    m_ModeClock(0),
    m_pVSyncCallback(nullptr),
    m_LCDControl(0x00),
    m_ScrollY(0x00),
    m_ScrollX(0x00),
    m_LCDControllerYCoordinate(0x00),
    m_LYCompare(0x00),
    m_WindowYPosition(0x00),
    m_WindowXPositionMinus7(0x00),
    m_BGPaletteData(0x00),
    m_ObjectPalette0Data(0x00),
    m_ObjectPalette1Data(0x00)
{
    SETMODE(ModeVBlank);
    memset(m_DisplayPixels, 0x00, ARRAYSIZE(m_DisplayPixels));
}

GPU::~GPU()
{
}

/*
    This method is called after the CPU executes an operation.  It tallies the number of cycles spent
    and ensures that the GPU switches between the various states listed below.  Essentially it loops
    through states 2, 3, and 0 until all 144 lines have been drawn. Then, it moves to state 1 where it
    cycles for 4560 cycles (10 lines @ 456 cycles per line). Finally, it starts over.

    The following are typical when the display is enabled:
    Mode 2  2_____2_____2_____2_____2_____2___________________2____
    Mode 3  _33____33____33____33____33____33__________________3___
    Mode 0  ___000___000___000___000___000___000________________000
    Mode 1  ____________________________________11111111111111_____

    The Mode Flag goes through the values 0, 2, and 3 at a cycle of about 109uS. 0 is present about
    48.6uS, 2 about 19uS, and 3 about 41uS. This is interrupted every 16.6ms by the VBlank (1).
    The mode flag stays set at 1 for about 1.08 ms.

    Mode 0 is present between 201-207 clks, 2 about 77-83 clks, and 3 about 169-175 clks. A complete
    cycle through these states takes 456 clks. VBlank lasts 4560 clks. A complete screen refresh occurs
    every 70224 clks.)
*/
void GPU::Step(unsigned long cycles)
{
    // If the LCD screen is off, then reset all values and exit early
    if (!IsLCDDisplayEnabled)
    {
        m_LCDControllerYCoordinate = 153;
        m_ModeClock = VBlankCycles;
        SETMODE(ModeVBlank);
        return;
    }

    m_ModeClock += cycles;

    switch (GETMODE)
    {
    case ModeReadingOAM:
        // OAM Read mode. Scanline active.
        if (m_ModeClock >= ReadingOAMCycles)
        {
            m_ModeClock -= ReadingOAMCycles;
            SETMODE(ModeReadingOAMVRAM);
        }
        break;
    case ModeReadingOAMVRAM:
        // VRAM Read mode. Scanline active.
        if (m_ModeClock >= ReadingOAMVRAMCycles)
        {
            m_ModeClock -= ReadingOAMVRAMCycles;
            SETMODE(ModeHBlank);

            // Write a scanline to the framebuffer
            RenderScanline();

            if (HBlankInterrupt && (m_CPU != nullptr))
            {
                m_CPU->TriggerInterrupt(INT48);
            }
        }
        break;
    case ModeHBlank:
        // End of scan line.
        if (m_ModeClock >= HBlankCycles)
        {
            m_ModeClock -= HBlankCycles;

            // After the last HBlank, push the framebuffer to the window
            m_LCDControllerYCoordinate++;
            if (m_LCDControllerYCoordinate == 144)
            {
                // Enter VBlank and render framebuffer
                SETMODE(ModeVBlank);
                RenderImage();

                if (m_CPU != nullptr)
                {
                    m_CPU->TriggerInterrupt(INT40);
                    if (VBlankInterrupt)
                    {
                        m_CPU->TriggerInterrupt(INT48);
                    }
                }
            }
            else
            {
                // Move onto next line
                SETMODE(ModeReadingOAM);
            }
        }
        break;
    case ModeVBlank:
        if (m_ModeClock >= VBlankCycles)
        {
            m_ModeClock -= VBlankCycles;

            // VBlank for 10 lines
            m_LCDControllerYCoordinate++;
            if (m_LCDControllerYCoordinate == 154)
            {
                // Go back to the top left
                SETMODE(ModeReadingOAM);
                m_LCDControllerYCoordinate = 0x00;
            }
        }
        break;
    }

    // Bit 2 - Coincidence Flag  (0:LYC<>LY, 1:LYC=LY) (Read Only)
    if (m_LYCompare == m_LCDControllerYCoordinate)
    {
        m_LCDControllerStatus = SETBIT(m_LCDControllerStatus, 2);

        if (LYCoincidenceInterrupt && (m_CPU != nullptr))
        {
            m_CPU->TriggerInterrupt(INT48);
        }
    }
    else
    {
        m_LCDControllerStatus = CLEARBIT(m_LCDControllerStatus, 2);
    }
}

byte* GPU::GetCurrentFrame()
{
    return m_DisplayPixels;
}

// IMemoryUnit
byte GPU::ReadByte(const ushort& address)
{
    if (address >= 0x8000 && address <= 0x9FFF)
    {
        // CONSIDER: Test mode to see if available
        if (IsLCDDisplayEnabled && (GETMODE == ModeReadingOAMVRAM))
        {
            //Logger::Log("GPU:ReadByte cannot access VRAM.");
            return 0x00;
        }

        return m_VRAM[address - 0x8000];
    }
    else if (address >= 0xFE00 && address <= 0xFE9F)
    {
        // CONSIDER: Test mode to see if available
        if (IsLCDDisplayEnabled && (GETMODE == ModeReadingOAM || GETMODE == ModeReadingOAMVRAM))
        {
            //Logger::Log("GPU:ReadByte cannot access OAM.");
            return 0x00;
        }

        return m_OAM[address - 0xFE00];
    }

    switch (address)
    {
    case LCDControl:
        return m_LCDControl;
    case LCDControllerStatus:
        return m_LCDControllerStatus;
    case ScrollY:
        return m_ScrollY;
    case ScrollX:
        return m_ScrollX;
    case LCDControllerYCoordinate:
        return m_LCDControllerYCoordinate;
    case LYCompare:
        return m_LYCompare;
    case WindowYPosition:
        return m_WindowYPosition;
    case WindowXPositionMinus7:
        return m_WindowXPositionMinus7;
    case BGPaletteData:
        return m_BGPaletteData;
    case ObjectPalette0Data:
        return m_ObjectPalette0Data;
    case ObjectPalette1Data:
        return m_ObjectPalette1Data;
    case DMATransferAndStartAddress:
        Logger::Log("GPU::ReadByte cannot read from address 0x%04X (DMATransferAndStartAddress)", address);
        return 0x00;
    default:
        Logger::Log("GPU::ReadByte cannot read from address 0x%04X", address);
        return 0x00;
    }
}

bool GPU::WriteByte(const ushort& address, const byte val)
{
    if (address >= 0x8000 && address <= 0x9FFF)
    {
        if (IsLCDDisplayEnabled && (GETMODE == ModeReadingOAMVRAM))
        {
            //Logger::Log("GPU:WriteByte cannot access VRAM.");
            return false;
        }

        m_VRAM[address - 0x8000] = val;
        return true;
    }
    else if (address >= 0xFE00 && address <= 0xFE9F)
    {
        if (IsLCDDisplayEnabled && (GETMODE == ModeReadingOAM || GETMODE == ModeReadingOAMVRAM))
        {
            //Logger::Log("GPU:WriteByte cannot access OAM.");
            return false;
        }

        m_OAM[address - 0xFE00] = val;
        return true;
    }

    switch (address)
    {
    case LCDControl:
        m_LCDControl = val;
        return true;
    case LCDControllerStatus:
        // Bits 7-3 are writable, Bits 2-0 are read only
        m_LCDControllerStatus = (val & 0xF8) | (m_LCDControllerStatus & 0x07);
        return true;
    case ScrollY:
        m_ScrollY = val;
        return true;
    case ScrollX:
        m_ScrollX = val;
        return true;
    case LCDControllerYCoordinate:
        m_LCDControllerYCoordinate = 0;
        return true;
    case LYCompare:
        m_LYCompare = val;
        return true;
    case WindowYPosition:
        m_WindowYPosition = val;
        return true;
    case WindowXPositionMinus7:
        m_WindowXPositionMinus7 = val;
        return true;
    case BGPaletteData:
        m_BGPaletteData = val;
        return true;
    case ObjectPalette0Data:
        m_ObjectPalette0Data = val;
        return true;
    case ObjectPalette1Data:
        m_ObjectPalette1Data = val;
        return true;
    case DMATransferAndStartAddress:
        LaunchDMATransfer(val);
        return true;
    default:
        Logger::Log("GPU::WriteByte cannot write to address 0x%04X", address);
        return false;
    }
}

void GPU::SetVSyncCallback(void(*pCallback)())
{
    m_pVSyncCallback = pCallback;
}

void GPU::PreBoot()
{
    m_LCDControllerYCoordinate = 0x91;
    m_ScrollY = 0x00;
    m_ScrollX = 0x00;
    m_LYCompare = 0x00;
    m_BGPaletteData = 0xFC;
    m_ObjectPalette0Data = 0xFF;
    m_ObjectPalette1Data = 0xFF;
    m_WindowYPosition = 0x00;
    m_WindowXPositionMinus7 = 0x00;
}

void GPU::LaunchDMATransfer(const byte address)
{
    /*
    DMA Transfer and Start Address
    Writing to this register launches a DMA transfer from ROM or RAM to OAM memory (sprite attribute
    table). The written value specifies the transfer source address divided by 100h, ie. source &
    destination are:

    Source:      XX00-XX9F   ;XX in range from 00-F1h
    Destination: FE00-FE9F
    */

    ushort source = (static_cast<ushort>(address) * 0x0100);
    for (byte offset = 0x00; offset <= 0x9F; offset++)
    {
        m_OAM[offset] = m_MMU->ReadByte(source | offset);
    }
}

void GPU::RenderScanline()
{
    RenderBackgroundScanline();

    if (WindowDisplayEnable)
    {
        // RenderWindowsScanline();
    }

    if (OBJDisplayEnable)
    {
        RenderOBJScanline();
    }
}

void GPU::RenderImage()
{
    if (m_pVSyncCallback != nullptr)
    {
        m_pVSyncCallback();
    }
}

void GPU::RenderBackgroundScanline()
{
    const byte palette[]
    {
        GBColors[m_BGPaletteData & 0x03],
        GBColors[(m_BGPaletteData >> 2) & 0x03],
        GBColors[(m_BGPaletteData >> 4) & 0x03],
        GBColors[(m_BGPaletteData >> 6) & 0x03],
    };

    if (!BGDisplayEnable)
    {
        /*
        LCDC.0 - 1) Monochrome Gameboy and SGB: BG Display
        When Bit 0 is cleared, the background becomes blank (white).
        Window and Sprites may still be displayed (if enabled in Bit 1 and/or Bit 5).
        */
        for (int x = 0;x < 160;x++)
        {
            // If BG is disabled, render a white background
            m_DisplayPixels[(m_LCDControllerYCoordinate * 160) + x] = GBColors[0];
        }

        return;
    }

    /*
    Bit 4 - BG & Window Tile Data Select   (0=8800-97FF, 1=8000-8FFF)
    Bit 3 - BG Tile Map Display Select     (0=9800-9BFF, 1=9C00-9FFF)
    #define BGWindowTileDataSelect ISBITSET(m_LCDControl, 4)
    #define BGWTileMapDisplaySelect ISBITSET(m_LCDControl, 3)
    */
    ushort tileNumberMap = BGWTileMapDisplaySelect ? 0x9C00 : 0x9800;
    ushort tileData = BGWindowTileDataSelect ? 0x8000 : 0x9000;

    byte tileY = (byte)(((m_LCDControllerYCoordinate + m_ScrollY) / 8) % 32);
    byte tileYOffset = (byte)((m_LCDControllerYCoordinate + m_ScrollY) % 8);
    for (byte x = 0; x < 160; x++)
    {
        byte tileX = (byte)(((m_ScrollX + x) / 8) % 32);
        byte tileNumber = ReadByte((ushort)(tileNumberMap + (tileY * 32) + tileX));
        ushort tileDataPtr = 0;

        if (BGWindowTileDataSelect)
        {
            tileDataPtr = (ushort)(tileData + tileNumber * 0x10);
        }
        else
        {
            tileDataPtr = (ushort)(tileData + static_cast<sbyte>(tileNumber) * 0x10);
        }

        tileDataPtr += (ushort)(tileYOffset * 2);

        byte b1 = ReadByte(tileDataPtr);
        byte b2 = ReadByte((ushort)(tileDataPtr + 1));

        byte bit = (byte)(7 - ((m_ScrollX + x) % 8));
        byte pLo = ISBITSET(b1, bit) ? 0x01 : 0x00;
        byte pHi = ISBITSET(b2, bit) ? 0x02 : 0x00;

        byte color = palette[pLo + pHi];

        int index = (m_LCDControllerYCoordinate * 160) + x;
        m_DisplayPixels[index] = color;
    }
}

void GPU::RenderOBJScanline()
{
    const byte SPRITESIZEINBYTES = 16;

    // Loop through each sprite
    for (byte i = 0; i < 160; i += 4)
    {
        // Grab the sprite data
        byte objY = m_OAM[i];                // The sprite Y position, minus 16 (apparently)
        byte spriteSize = OBJSize ? 0x10 : 0x08; // 0x00 = 8x8, 0x01 = 8x16
        int height = spriteSize;

        // Sprite rect:
        // x = spriteX - 8
        // y = spriteY - 16
        // w = 8
        // h = spriteSize

        int y = objY - 16;

        // Check if the sprite is on the current scanline
        if ((y <= m_LCDControllerYCoordinate) && ((y + height) > m_LCDControllerYCoordinate))
        {
            byte objX = m_OAM[i + 1];               // The sprite X position, minus 8 (apparently)
            byte spriteTileNumber = m_OAM[i + 2];   // The tile or pattern number of the sprite
            byte spriteFlags = m_OAM[i + 3];        // The sprites render flags (priority, flip, palette)

            if (spriteSize == 0x10)
            {
                spriteTileNumber &= 0xFE;
            }

            byte paletteNumber = ISBITSET(spriteFlags, 4) ? 0x01 : 0x00;

            int x = objX - 8;

            const ushort tileData = 0x8000;

            // Create the palette to use for this sprite
            byte palette[]
            {
                0x00, // Unused - Transparent
                GBColors[(paletteNumber == 0x00) ? (m_ObjectPalette0Data >> 2 & 0x03) : (m_ObjectPalette1Data >> 2 & 0x03)],
                GBColors[(paletteNumber == 0x00) ? (m_ObjectPalette0Data >> 4 & 0x03) : (m_ObjectPalette1Data >> 4 & 0x03)],
                GBColors[(paletteNumber == 0x00) ? (m_ObjectPalette0Data >> 6 & 0x03) : (m_ObjectPalette1Data >> 6 & 0x03)]
            };

            // The memory location of this sprites tile can be found by adding the sprites tile
            // number to the location of the tile data.
            // If the spriteSize == 0x00, ignore the lower bit of the tile number.
            ushort tilePointer = tileData + (spriteTileNumber * SPRITESIZEINBYTES);
            byte tileYOffset = ISBITSET(spriteFlags, 6) ? ((height - 1) - (m_LCDControllerYCoordinate - y)) : (m_LCDControllerYCoordinate - y);
            tilePointer += (tileYOffset * 2);

            // The data for this line of the sprite, 8 pixels
            byte low = ReadByte(tilePointer);
            byte high = ReadByte((ushort)(tilePointer + 1));

            // Loop through all 8 pixels of this line
            for (int indexX = 0; indexX < 8; indexX++)
            {
                int pixelX = x + indexX;
                // Check if the pixel is still on screen
                if (pixelX >= 0 && pixelX < 160)
                {
                    byte bit = ISBITSET(spriteFlags, 5) ? indexX : 7 - indexX;
                    byte pixelVal = 0x00;
                    if (ISBITSET(high, bit)) pixelVal |= 0x02;
                    if (ISBITSET(low, bit)) pixelVal |= 0x01;
                    byte color = palette[pixelVal];

                    // If two sprites x coordinates are the same on DMG OR CGB, the one with the lower address in OAM will be 'on top'
                    // If two sprites x coordinates are different on DMG, the one with the x coordinate closer to the ? right ? of the screen will be on top, regardless of position in OAM. (When in DMG mode(i.e.when playing a non - color enhanced game), the CGB emulates this behavior)
                    // If two sprites x coordinates are different on CGB in CGB mode, the one with the lower address in OAM will be 'on top', regardless of x coordinate.

                    // If the pixel is not transparent
                    if (pixelVal != 0x00)
                    {
                        int index = (m_LCDControllerYCoordinate * 160) + pixelX;

                        // If the sprite has priority 0 (Render above BG)
                        if (!ISBITSET(spriteFlags, 7))
                        {
                            m_DisplayPixels[index] = color;
                        }
                        else 
                        {
                            // This sprite has priority 1 (Render behind BG)
                            // The sprite pixels only get rendered above BG pixels that are white.
                            // All other BG pixels stay on top.

                            // If the BG pixel is white
                            if (m_DisplayPixels[index] == 0x00)
                            {
                                // Render that sprite pixel
                                m_DisplayPixels[index] = color;
                            }
                        }
                    }
                }
            }
        }
    }
}
