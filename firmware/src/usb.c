/**
 * STM32 USB Peripheral Driver
 *
 * Core usb functionality
 *
 * Kevin Cuzner
 */

#include "usb.h"
#include "osc.h"
#include "stm32l0xx.h"

#include <stdint.h>
#include <string.h>

/**
 * Minimally sized data type for things in the PMA
 */
typedef uint16_t PMAWord;

//single ended buffer descriptor
typedef struct __attribute__((packed)) {
    PMAWord tx_addr;
    PMAWord tx_count;
    PMAWord rx_addr;
    PMAWord rx_count;
} USBBufferDescriptor;

/**
 * Endpoint status, tracked here to enable easy sending and receiving through
 * USB by the application program.
 *
 * size: Endpoint packet size in PMA (buffer table contains PMA buffer addresses)
 *
 * tx_buf: Start of transmit buffer located in main memory
 * tx_pos: Current transmit position within the buffer or zero if transmission is finished
 * tx_len: Transmit buffer length in bytes
 *
 * rx_buf: Start of receive buffer located in main memory
 * rx_pos: Current receive position within the buffer
 */
typedef struct {
    uint16_t size; //endpoint packet size
    void *tx_buf; //transmit buffer located in main memory
    void *tx_pos; //next transmit position in the buffer or zero if done
    uint16_t tx_len; //transmit buffer length
    void *rx_buf; //receive buffer located in main memory
    void *rx_pos; //next transmit position in the buffer or zero if done
    uint16_t rx_len; //receive buffer length
} USBEndpointStatus;

typedef struct {
    union {
        uint16_t wRequestAndType;
        struct {
            uint8_t bmRequestType;
            uint8_t bRequest;
        };
    };
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} USBSetupPacket;

typedef enum { TOK_SETUP, TOK_IN, TOK_OUT } USBToken;

#define USB_ENDPOINT_REGISTER(ENDP) (*((&USB->EP0R) + ((ENDP) << 1)))

// Packet memory area is managed by a combination of the linker script and the
// following attributes applied to each symbol placed into the PMA.
//
// All symbols in the packet area need a minimum word alignment. This is
// enforced by registers pointing to the PMA always having their LSB forced to
// zero by hardware.
//
// The buffer table is required to be 8-byte aligned due to the 3 LSB of the
// BTABLE register being forced to zero.
//
// Better space usage may be attained if the buffer descriptor table is
// declared first since it was observed that GCC doesn't always reposition
// symbols for best space utilization.
//
// Note that the PMA must always be accessed by single or double bytes, never
// by four bytes. This differs from the STM32F1 series USB IP which requires
// translation to 32-bit aligned accesses in order to read or write the PMA.
// The method used in the STM32L0 IP does not require any translation and can
// be directly accessed by the application code.

#define PMA_SECTION ".pma,\"aw\",%nobits//" //a bit of a hack to prevent .pma from being programmed
#define _PMA __attribute__((section (PMA_SECTION), aligned(2))) //everything needs to be 2-byte aligned
#define _PMA_BDT __attribute__((section (PMA_SECTION), used, aligned(8))) //buffer descriptors need to be 8-byte aligned

/**
 * Buffer table located in packet memory. This table contains structures which
 * describe the buffer locations for the 8 endpoints in packet memory.
 */
static USBBufferDescriptor _PMA_BDT bt[8];

/**
 * Translates a PMA pointer into a local address for the USB peripheral
 */
#define USB_LOCAL_ADDR(PMAPTR) (uint16_t)((uint32_t)(PMAPTR) - USB_PMAADDR)
/**
 * Translates a USB local address into a PMA pointer
 */
#define PMA_ADDR_FROM_USB_LOCAL(LOCALPTR) (PMAWord *)((LOCALPTR) + USB_PMAADDR)

/**
 * Placeholder for address translation between PMA space and Application space.
 * Unused on the STM32L0
 */
#define APPLICATION_ADDR(PMAPTR) (uint16_t *)(PMAPTR)

/**
 * Placeholder for size translation between PMA space and application space.
 * Unused on the STM32L0
 */
#define APPLICATION_SIZEOF(S) (sizeof(S))

/**
 * Start of the wide open free packet memory area, provided by the linker script
 */
extern PMAWord _pma_end;

/**
 * Current memory break in PMA space (note that the pointer itself it is stored
 * in normal memory).
 *
 * On usb reset all packet buffers are considered deallocated and this resets
 * back to the _pma_end address. This is a uint16_t because all address in
 * PMA must be 2-byte aligned if they are to be used in an endpoint buffer.
 */
static PMAWord *pma_break;

/**
 * Holds the current setup status for an endpoint
 */
static USBEndpointStatus endpoint_status[8];

/**
 * Holds the information for the received setup packet
 */
static USBSetupPacket last_setup;

void __attribute__ ((weak)) hook_usb_reset(void) { }
void __attribute__ ((weak)) hook_usb_sof(void) { }
void __attribute__ ((weak)) hook_usb_set_configuration(uint8_t configuration) { }
void __attribute__ ((weak)) hook_usb_set_interface(uint8_t configuration, uint8_t interface) { }
void __attribute__ ((weak)) hook_usb_endpoint_received(uint8_t endpoint, void *buf, uint16_t len) { }
void __attribute__ ((weak)) hook_usb_endpoint_sent(uint8_t endpoint, void *buf, uint16_t len) { }

/**
 * Initializes the USB peripheral
 */
void usb_init(void)
{
    //Set RC48 as the HSI48 source
    RCC->CCIPR |= RCC_CCIPR_HSI48SEL;
    //
    //Enable module clocks
    RCC->APB1ENR |= RCC_APB1ENR_USBEN | RCC_APB1ENR_CRSEN;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
}

/**
 * Enables or disables the USB peripheral
 */
void usb_enable(void)
{
    //Enable the VREF for HSI48
    SYSCFG->CFGR3 |= 0x01;
    while (!(SYSCFG->CFGR3 & SYSCFG_CFGR3_VREFINT_RDYF)) { }
    SYSCFG->CFGR3 |= SYSCFG_CFGR3_ENREF_HSI48;
    while (!(SYSCFG->CFGR3 & SYSCFG_CFGR3_REF_HSI48_RDYF)) { }

    //Enable HSI48
    RCC->CRRCR |= RCC_CRRCR_HSI48ON;
    while (!(RCC->CRRCR & RCC_CRRCR_HSI48RDY)) { }

    //reset the peripheral
    USB->CNTR = USB_CNTR_FRES;

    //enable pullup to signal to the host that we are here
    USB->BCDR |= USB_BCDR_DPPU;

    //clear interrupts
    USB->ISTR = 0;

    //Enable the USB interrupt
    NVIC_EnableIRQ(USB_IRQn);

    USB->CNTR = USB_CNTR_RESETM; //enable the USB reset interrupt
}

void usb_disable(void)
{
    //reset the peripheral
    USB->CNTR = USB_CNTR_FRES;

    //clear interrupts
    USB->ISTR = 0;
    USB->CNTR = USB_CNTR_FRES | USB_CNTR_LPMODE | USB_CNTR_PDWN; //power down usb peripheral

    //disable pullup
    USB->BCDR &= ~USB_BCDR_DPPU;

    //disable clock recovery system
    CRS->CR &= ~CRS_CR_CEN;

    //Power down the HSI48 clock
    RCC->CRRCR &= ~RCC_CRRCR_HSI48ON;

    //Disable the VREF for HSI48
    SYSCFG->CFGR3 &= ~SYSCFG_CFGR3_ENREF_HSI48;
}

/**
 * Dynamically allocates a buffer from the PMA
 * len: Buffer length in bytes
 *
 * Returns PMA buffer address
 */
static PMAWord *usb_allocate_pma_buffer(uint16_t len)
{
    PMAWord *buffer = pma_break;

    //move the break, ensuring that the next buffer doesn't collide with this one
    len = (len + 1) / sizeof(PMAWord); //divide len by sizeof(PMAWord), rounding up (should be optimized to a right shift)
    pma_break += len; //mmm pointer arithmetic (pma_break is the appropriate size to advance the break correctly)

    return buffer;
}

/**
 * Sets the status bits to the appropriate value, preserving non-toggle fields
 *
 * endpoint: Endpoint register to modify
 * status: Desired value of status bits (i.e. USB_EP_TX_DIS, USB_EP_RX_STALL, etc)
 * tx_rx_mask: Mask indicating which bits are being modified (USB_EPTX_STAT or USB_EPRX_STAT)
 */
static inline void usb_set_endpoint_status(uint8_t endpoint, uint32_t status, uint32_t tx_rx_mask)
{
    uint32_t val = USB_ENDPOINT_REGISTER(endpoint);
    USB_ENDPOINT_REGISTER(endpoint) = (val ^ (status & tx_rx_mask)) & (USB_EPREG_MASK | tx_rx_mask);
}

void usb_endpoint_setup(uint8_t endpoint, uint8_t address, uint16_t size, USBEndpointType type)
{
    if (endpoint > 7 || type > USB_ENDPOINT_INTERRUPT)
        return; //protect against tomfoolery

    endpoint_status[endpoint].size = size;
    USB_ENDPOINT_REGISTER(endpoint) = (type == USB_ENDPOINT_BULK ? USB_EP_BULK :
            type == USB_ENDPOINT_CONTROL ? USB_EP_CONTROL :
            USB_EP_INTERRUPT) |
        (address & 0xF);
}

/**
 * Performs a copy from a region of memory into a the PMA
 *
 * src: Pointer to source located in normal memory
 * pmaDest: Pointer to destination located in PMA
 * len: Length in bytes to copy
 */
static void usb_pma_copy_in(void *src, PMAWord *pmaDest, uint16_t len)
{
    //note the sizes of the following
    PMAWord *wordSrc = (PMAWord *)src;
    uint16_t *appDest = APPLICATION_ADDR(pmaDest);

    for (uint16_t i = 0; i < len; i += sizeof(PMAWord)) //we move along by word
    {
        *appDest = *wordSrc;
        appDest++; //move along by four bytes to next PMA word
        wordSrc++; //move along by one word
    }
}

/**
 * Performs a copy from the PMA into a region of memory
 *
 * pmaSrc: Pointer to source located in PMA
 * dest: Pointer to destination located in normal memory
 * len: Length in bytes to copy
 */
static void usb_pma_copy_out(PMAWord *pmaSrc, void *dest, uint16_t len)
{
    //note the size of the following
    uint16_t *appSrc = APPLICATION_ADDR(pmaSrc);
    PMAWord *wordDest = (PMAWord *)dest;

    for (uint16_t i = 0; i < len; i += sizeof(PMAWord)) //we move along by word
    {
        *wordDest = *appSrc;
        wordDest++; //move along by four bytes to next PMA word
        appSrc++; //move along by one word
    }
}

/**
 * Sends the next packet for the passed endpoint. If there is no remaining data
 * to send, no operation occurs.
 *
 * endpoint: Endpoint to send a packet on
 */
static void usb_endpoint_send_next_packet(uint8_t endpoint)
{
    uint16_t packetSize = endpoint_status[endpoint].size;

    //is transmission finished (or never started)?
    if (!endpoint_status[endpoint].tx_pos || !packetSize)
        return;

    //if we get this far, we have something to transmit, even if its nothing

    //check for PMA buffer presence, allocate if needed
    if (!*APPLICATION_ADDR(&bt[endpoint].tx_addr))
    {
        *APPLICATION_ADDR(&bt[endpoint].tx_addr) = USB_LOCAL_ADDR(usb_allocate_pma_buffer(packetSize));
    }

    //determine actual packet length, capped at the packet size
    uint16_t completedLength = endpoint_status[endpoint].tx_pos - endpoint_status[endpoint].tx_buf;
    uint16_t len = endpoint_status[endpoint].tx_len - completedLength;
    if (len > packetSize)
        len = packetSize;

    //copy to PMA tx buffer
    uint16_t localBufAddr = *APPLICATION_ADDR(&bt[endpoint].tx_addr);
    usb_pma_copy_in(endpoint_status[endpoint].tx_pos, PMA_ADDR_FROM_USB_LOCAL(localBufAddr), len);

    //set count to actual packet length
    *APPLICATION_ADDR(&bt[endpoint].tx_count) = len;

    //move tx_pos
    endpoint_status[endpoint].tx_pos += len;

    //There are now three cases:
    // 1. We still have bytes to send
    // 2. We have sent all bytes and len == packetSize
    // 3. We have sent all bytes and len != packetSize
    //
    //Case 1 obviously needs another packet. Case 2 needs a zero length packet.
    //Case 3 should result in no further packets and the application being
    //notified once the packet being queued here is completed.
    //
    //Responses:
    // 1. We add len to tx_pos. On the next completed IN token, this function
    //    will be called again.
    // 2. We add len to tx_pos. On the next completed IN token, this function
    //    will be called again. A zero length packet will then be produced.
    //    Since len will not equal packetSize at that point, Response 3 will
    //    happen.
    // 3. We now set tx_pos to zero. On the next completed IN token, the
    //    application can be notified. Further IN tokens will result in a NAK
    //    condition which will prevent repeated notifications. Further calls to
    //    this function will result in no operation until usb_endpoint_send is
    //    called again.
    if (len != packetSize)
    {
        endpoint_status[endpoint].tx_pos = 0;
    }
    else
    {
        endpoint_status[endpoint].tx_pos += len;
    }

    //Inform the endpoint that the packet is ready.
    usb_set_endpoint_status(endpoint, USB_EP_TX_VALID, USB_EPTX_STAT);
}

void usb_endpoint_send(uint8_t endpoint, void *buf, uint16_t len)
{
    //TODO: Race condition here since usb_endpoint_send_next_packet is called during ISRs.
    if (buf)
    {
        endpoint_status[endpoint].tx_buf = buf;
        endpoint_status[endpoint].tx_len = len;
        endpoint_status[endpoint].tx_pos = buf;
        usb_endpoint_send_next_packet(endpoint);
    }
    else
    {
        endpoint_status[endpoint].tx_pos = 0;
        usb_set_endpoint_status(endpoint, USB_EP_TX_DIS, USB_EPTX_STAT);
    }
}

/**
 * Begins a packet receive operation by preparing the buffer
 */
static void usb_endpoint_begin_packet_receive(uint8_t endpoint)
{
    uint16_t packetSize = endpoint_status[endpoint].size;

    //is reception finished (or never started)?
    if (!endpoint_status[endpoint].rx_pos || !packetSize)
        return;

    //if we get this far, we have a space to ready receive into, even if its 0 bytes long

    //check for PMA buffer presence, allocate if needed
    if (!*APPLICATION_ADDR(&bt[endpoint].rx_addr))
    {
        *APPLICATION_ADDR(&bt[endpoint].rx_addr) = USB_LOCAL_ADDR(usb_allocate_pma_buffer(packetSize));
        //set buffer size in blocks, rounding down to prevent overrun (workaround: Don't allocate buffers with sizes that will be misrepresented here)
        uint16_t blocks = packetSize >> 1;
        if (packetSize > 62)
        {
            blocks >>= 4;
            blocks |= 0x20; //set BLSIZE bit when we shift this up
        }
        else
        {
            blocks &= 0x001F; //reset BLSIZE bit when we shift this up
        }
        blocks -= 1;
        *APPLICATION_ADDR(&bt[endpoint].rx_count) = blocks << 10;
    }

    //Inform the endpoint that we have space to receive into
    usb_set_endpoint_status(endpoint, USB_EP_RX_VALID, USB_EPRX_STAT);
}

uint32_t usbtemp[32];
uint8_t usbtempi;

/**
 * Processes a received block of data, starting a new receive operation if necessary
 * Call when an OUT is completed.
 */
static void usb_endpoint_end_packet_receive(uint8_t endpoint)
{
    uint16_t packetSize = endpoint_status[endpoint].size;

    //received vs len:
    //
    //The received count reflects exactly how many bytes were received by
    //the peripheral. On the other hand, len reflects how many bytes from
    //that reception can actually fit into the memory buffer. All comparisons
    uint16_t received = *APPLICATION_ADDR(&bt[endpoint].rx_count) & 0x1FF;
    uint16_t completedLength = endpoint_status[endpoint].rx_pos - endpoint_status[endpoint].rx_buf;
    //len is the number of bytes to copy so that we don't overrun memory if we receive too much data
    uint16_t len = endpoint_status[endpoint].rx_len - completedLength;
    if (len > received)
        len = received;
    uint16_t localBufAddr = *APPLICATION_ADDR(&bt[endpoint].rx_addr);
    usb_pma_copy_out(PMA_ADDR_FROM_USB_LOCAL(localBufAddr), endpoint_status[endpoint].rx_pos, len);

    //There are now three cases:
    // 1. We still have bytes to receive
    // 2. We have received all bytes and len == packetSize
    // 3. We have received all bytes and len != packetSize
    //
    //Case 1 obviously needs another packet. Case 2 needs a zero length packet.
    //Case 3 should result in no further packets and the application being
    //notified once the packet being queued here is completed.
    //
    //Responses:
    // 1. We add len to rx_pos. On the next completed OUT token, this function
    //    will be called again. Note that len may be zero if the buffer is out
    //    of room.
    // 2. We add len to rx_pos. On the next completed OUT token, this function
    //    will be called again. A zero length packet should be received.
    //    Since received will not equal packetSize at that point, Response 3
    //    will happen.
    // 3. We now set rx_pos to zero. On the next completed OUT token, the
    //    application can be notified. Further OUT tokens will result in a NAK
    //    condition which will prevent repeated notifications. Further calls to
    //    this function will result in no operation until usb_endpoint_receive is
    //    called again.
    if (received != packetSize) //use received instead of len so we react correctly to actual events
    {
        //this is the end of reception. We no longer will receive.
        endpoint_status[endpoint].rx_pos = 0;
    }
    else
    {
        //receive at the next spot in the buffer
        endpoint_status[endpoint].rx_pos += len; //use len instead of received so we don't overrun
        usb_endpoint_begin_packet_receive(endpoint);
    }

}

void usb_endpoint_receive(uint8_t endpoint, void *buf, uint16_t len)
{
    if (buf)
    {
        endpoint_status[endpoint].rx_buf = buf;
        endpoint_status[endpoint].rx_pos = buf;
        endpoint_status[endpoint].rx_len = len;
        usb_endpoint_begin_packet_receive(endpoint);
    }
    else
    {
        endpoint_status[endpoint].rx_pos = 0;
        usb_set_endpoint_status(endpoint, USB_EP_RX_DIS, USB_EPRX_STAT);
    }
}

/**
 * Called during interrupt for a usb reset
 */
static void usb_reset(void)
{
    //clear all interrupts
    USB->ISTR = 0;
    
    //enable clock recovery system
    CRS->CR |= CRS_CR_AUTOTRIMEN | CRS_CR_CEN;


    //BDT lives at the beginning of packet memory (see linker script)
    USB->BTABLE = USB_LOCAL_ADDR(bt);

    //All packet buffers are now deallocated and considered invalid. All endpoints statuses are reset.
    memset(APPLICATION_ADDR(bt), 0, APPLICATION_SIZEOF(bt));
    memset(endpoint_status, 0, sizeof(endpoint_status));
    pma_break = &_pma_end;
    if (!pma_break)
        pma_break++; //we use the assumption that 0 = none = invalid all over

    //Set up endpoint 0 as a 64-byte control endpoint
    //TODO: Dynamically determine the endpoint size from the descriptor
    usb_endpoint_setup(0, 0, USB_CONTROL_ENDPOINT_SIZE, USB_ENDPOINT_CONTROL);
    usb_endpoint_receive(0, &last_setup, sizeof(last_setup));

    //Perform any application reset functions
    hook_usb_reset();

    //enable correct transfer and reset interrupts
    USB->CNTR = USB_CNTR_CTRM | USB_CNTR_RESETM | USB_CNTR_SOFM | USB_CNTR_ERRM | USB_CNTR_PMAOVRM;

    //Reset USB address to 0 with the device enabled
    USB->DADDR = USB_DADDR_EF;
}

static void usb_endp0_setup(void)
{
    switch (last_setup.wRequestAndType)
    {
        case 0x0680:
        case 0x0681:
            leds_set_center(1, 0, 0);
            break;
        case 0x0580:
            leds_set_center(0, 1, 0);
            break;
        default:
            break;
    }
}

/**
 * Performs all control operations, delegating to the application if needed. Called at the
 * end of a token.
 *
 * token: Token type that has been completed and triggered the call to this function
 */
static void usb_handle_endp0(USBToken token)
{
    switch (token)
    {
        case TOK_SETUP:
            usb_endp0_setup();
            break;
        case TOK_IN:
            break;
        case TOK_OUT:
            break;
    }
}


void USB_IRQHandler(void)
{
    volatile uint16_t stat = USB->ISTR;
    if (stat & USB_ISTR_RESET)
    {
        usb_reset();
        hook_usb_reset();
        USB->ISTR = ~USB_ISTR_RESET;
    }
    if (stat & USB_ISTR_SUSP)
    {
        USB->ISTR = ~USB_ISTR_SUSP;
    }
    if (stat & USB_ISTR_WKUP)
    {
        USB->ISTR = ~USB_ISTR_WKUP;
    }
    if (stat & USB_ISTR_ERR)
    {
        USB->ISTR = ~USB_ISTR_ERR;
    }
    if (stat & USB_ISTR_SOF)
    {
        hook_usb_sof();
        USB->ISTR = ~USB_ISTR_SOF;
    }
    if (stat & USB_ISTR_ESOF)
    {
        USB->ISTR = ~USB_ISTR_ESOF;
    }
    if (stat & USB_ISTR_PMAOVR)
    {
        USB->ISTR = ~USB_ISTR_PMAOVR;
    }

    while ((stat = USB->ISTR) & USB_ISTR_CTR)
    {
        uint8_t endpoint = stat & USB_ISTR_EP_ID;
        uint16_t val = USB_ENDPOINT_REGISTER(endpoint);

        if (val & USB_EP_CTR_RX)
        {
            usbtemp[usbtempi++] = *APPLICATION_ADDR(&bt[endpoint].rx_count);
            usb_endpoint_end_packet_receive(endpoint);
            USB_ENDPOINT_REGISTER(endpoint) = val & USB_EPREG_MASK & ~USB_EP_CTR_RX;
            if (!endpoint_status[endpoint].rx_pos)
            {
                if (endpoint)
                {
                    hook_usb_endpoint_received(endpoint, endpoint_status[endpoint].rx_buf, endpoint_status[endpoint].rx_len);
                }
                else
                {
                    //endpoint 0 OUT or SETUP complete
                    usb_handle_endp0(val & USB_EP_SETUP ? TOK_SETUP : TOK_OUT);
                }
            }
        }

        if (val & USB_EP_CTR_TX)
        {
            usb_endpoint_send_next_packet(endpoint);
            USB_ENDPOINT_REGISTER(endpoint) = val & USB_EPREG_MASK & ~USB_EP_CTR_TX;
            if (!endpoint_status[endpoint].tx_pos)
            {
                if (endpoint)
                {
                    hook_usb_endpoint_sent(endpoint, endpoint_status[endpoint].tx_buf, endpoint_status[endpoint].tx_len);
                }
                else
                {
                    //endpoint 0 IN complete
                    usb_handle_endp0(TOK_IN);
                }
            }
        }
    }
}
