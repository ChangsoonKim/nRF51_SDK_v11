/*
This software is subject to the license described in the license.txt file included with this software distribution.
You may not use this file except in compliance with this license.
Copyright © Dynastream Innovations Inc. 2015
All rights reserved.
*/

#if defined(INCLUDE_DEBUG_CHANNEL)


#include <string.h>
#include "debug.h"
#include "ant_parameters.h"
#include "ant_interface.h"
#include "app_error.h"
#include "nrf_soc.h"
#include "ant_channel_config.h"

// Channel parameters
#define ANT_CHANNEL_DEFAULT_NETWORK           ((uint8_t) 0)         /**< Network number. */
#define DEBUG_RADIO_FREQUENCY                 ((uint8_t) 66)        /**< Radio frequency. */
#define DEBUG_CHANNEL_PERIOD                  ((uint16_t) 8192)     /**< Channel period (4Hz). */
#define DEBUG_DEVICE_TYPE                     ((uint8_t) 8)         /**< Device type. */
#define DEBUG_TRANSMISSION_TYPE               ((uint8_t) 1)         /**< Transmission type. */

// This index will not be displayed in ObservANT
#define ANT_DEBUG_FIELD_INVALID               ((uint8_t) 0xFF)      /**< Invalid field. */

// Indexes within a message
#define ANT_PAGE_INDEX                        (uint8_t)  0)         /**< Page index. */
#define ANT_FAST_DEBUG_BYTE_INDEX             ((uint8_t) 1)         /**< Fast debug byte index. */

// ANT Pages
#define ANT_DEBUG_PAGE_MESSAGE                ((uint8_t) 0xF9)      /**< Debug page. */

//Filter Commands
#define ANT_DEBUG_FILTER_COMMAND              ((uint8_t) 3)         /**< Debug filter command. */
#define ANT_DEBUG_SUB_COMMAND_FILTER_ADD      ((uint8_t) 1)         /**< Debug filter sub command: add. */
#define ANT_DEBUG_SUB_COMMAND_FILTER_CLEAR    ((uint8_t) 2)         /**< Debug filter sub command: clear. */

#define ANT_RESERVED_BYTE                     ((uint8_t) 0xFF)      /**< Reserved byte. */

// Misc. defines
#define ANT_PAGE_DEBUG_KEY0_INDEX           ((uint8_t) 2)           /**< Debug key index. */
#define ANT_PAGE_DEBUG_VALUE_SIZE           (sizeof(uint16_t) + sizeof(uint8_t))        /**< Debug value size. */

#define DEBUG_QUEUE_SIZE                    ((uint16_t) 256)        /**< Debug queue size. */


typedef struct
{
    uint8_t  index;     /**< Index. */
    uint16_t value;     /**< Value. */
    bool     output;    /**< Output. */
} debug_value_t;


static debug_value_t m_debug_queue[DEBUG_QUEUE_SIZE];               /**< List of debug fields (not really a queue). */
static uint8_t       m_hash_lookup[DEBUG_QUEUE_SIZE];               /**< This array depth has to span all possible indices. */
static uint8_t       m_queue_size;                                  /**< Queue size. */
static uint8_t       m_current_index_debug_channel;                 /**< Current index for debug channel. */
static uint8_t       m_head;                                        /**< Head. */
static bool          m_selective_debug;                             /**< Selective debug mode - only filtered fields are transmitted. */
static uint8_t       m_tx_buffer[ANT_STANDARD_DATA_PAYLOAD_SIZE];   /**< Tx buffer. */
static uint8_t       m_fast_debug_byte;                             /**< Fast debug byte. */

static custom_command_handler_t m_custom_command_callback = NULL;   /**< Custom command callback. */


/**
 * @brief Check if a certain index should be included in a debug message
 *
 * @param[in] current_index         Index to check
 *
 * @retval true         include
 * @retval false        do not include
 */
static bool ad_output_debug_mesg(uint8_t current_index)
{
    if(m_selective_debug)
    {
        return (m_debug_queue[current_index].output);
    }
    else
    {
        return(true);
    }
}

/**
 * @brief Constructs the next debug message to go out based on selective debugging
 *
 * @param[out] p_current_index      Current field index
 * @param[out] p_data               Destination of debug message
 */
static void ad_get_debug_page(uint8_t* p_data, uint8_t* p_current_index)
{
   uint8_t count = 0;

   p_data[0] = ANT_DEBUG_PAGE_MESSAGE;
   p_data[ANT_FAST_DEBUG_BYTE_INDEX] = m_fast_debug_byte;

   if(m_queue_size > 0)
   {
      // This is meant to handle both the priority search and selective debug
      // which are both independent methods of selecting which index should
      // go out next. So its probably more complicated than it needs to be.
      while(count < 2) // Two values per ANT message
      {
         uint8_t index = ANT_PAGE_DEBUG_KEY0_INDEX + (count * ANT_PAGE_DEBUG_VALUE_SIZE); // Index into ANT message

         if(ad_output_debug_mesg(*p_current_index)) // If ready to output based on the priority
         {
            p_data[index]   = m_debug_queue[*p_current_index].index;
            p_data[index+1] = (uint8_t) m_debug_queue[*p_current_index].value;
            p_data[index+2] = (uint8_t) (m_debug_queue[*p_current_index].value >> 8);
            count++;
         }
         *p_current_index = *p_current_index + 1;
         if(*p_current_index >= m_queue_size)
         {
            *p_current_index = 0;
         }
      }
   }
   else
   {
      memset(&p_data[ANT_PAGE_DEBUG_KEY0_INDEX], ANT_RESERVED_BYTE, 6);
   }
}


/**
 * @brief Sets the next broadcast message
 */
static void ad_update_tx(void)
{
    ad_get_debug_page(m_tx_buffer, &m_current_index_debug_channel);
    UNUSED_VARIABLE(sd_ant_broadcast_message_tx(DEBUG_CHANNEL, ANT_STANDARD_DATA_PAYLOAD_SIZE, m_tx_buffer));
}


/**
 * @brief Decode the command from an ANT event
 *
 * Debug filter command format:
 *
 * [F9][command][data1][data2][data3][data4][data5][data6]
 *
 * F9       - global ANT Debug message page
 * command  - sub-command
 * data     - data1 is start of sub-command definition
 *
 * @param[in] p_ant_evt The incomming event
 */
static void ad_decode_debug_command(ant_evt_t * p_ant_evt)
{
    ANT_MESSAGE* p_ant_message = (ANT_MESSAGE*)p_ant_evt->msg.evt_buffer;

    switch(p_ant_message->ANT_MESSAGE_aucPayload[1])
    {
        case ANT_DEBUG_FILTER_COMMAND:
        {

            if(p_ant_message->ANT_MESSAGE_aucPayload[2] == ANT_DEBUG_SUB_COMMAND_FILTER_ADD) // Set Filter
            {
                m_selective_debug = true;

                for (uint8_t index = 3; index < 8; index++) // For each arguement in the debug event (4)
                {
                   if(p_ant_message->ANT_MESSAGE_aucPayload[index] != ANT_DEBUG_FIELD_INVALID)
                   {
                      uint8_t key = m_hash_lookup[p_ant_message->ANT_MESSAGE_aucPayload[index]];

                      if(key == ANT_DEBUG_FIELD_INVALID)
                      {
                         // If key doesn't exist yet then add it to the buffer with a max value.
                         ad_set_debug_field(p_ant_message->ANT_MESSAGE_aucPayload[index],0xFFFF);

                         key = m_hash_lookup[p_ant_message->ANT_MESSAGE_aucPayload[index]];
                      }

                      m_debug_queue[key].output = true;
                   }
                }
             }
             else if(p_ant_message->ANT_MESSAGE_aucPayload[2] == ANT_DEBUG_SUB_COMMAND_FILTER_CLEAR) // Clear filter
             {
                m_selective_debug = false;

                for (uint8_t index = 0; index < m_queue_size; index++)
                {
                   m_debug_queue[index].output = false;
                }
             }
            break;
        }
        default:
        {
            break;
        }
    }
}


void ad_init(void)
{
    uint32_t err_code;

    m_queue_size                  = 0;
    m_current_index_debug_channel = 0;
    m_head                        = 0;
    m_selective_debug             = false;
    m_fast_debug_byte             = 0xFF;

    for (uint32_t i = 0; i < DEBUG_QUEUE_SIZE; i++)
    {
        m_debug_queue[i].index  = 0;
        m_debug_queue[i].value  = 0;
        m_hash_lookup[i]        = ANT_DEBUG_FIELD_INVALID;
    }

    ant_channel_config_t channel_config =
    {
        .channel_number     = DEBUG_CHANNEL,
        .channel_type       = CHANNEL_TYPE_MASTER,
        .ext_assign         = 0,
        .rf_freq            = DEBUG_RADIO_FREQUENCY,
        .transmission_type  = DEBUG_TRANSMISSION_TYPE,
        .device_type        = DEBUG_DEVICE_TYPE,
        .device_number      = (uint16_t) (NRF_FICR->DEVICEID[0]),
        .channel_period     = DEBUG_CHANNEL_PERIOD,
        .network_number     = ANT_CHANNEL_DEFAULT_NETWORK,
    };

    err_code = ant_channel_init(&channel_config);
    APP_ERROR_CHECK(err_code);

    err_code = sd_ant_channel_open(DEBUG_CHANNEL);
    APP_ERROR_CHECK(err_code);
}


void ad_register_custom_command_callback(custom_command_handler_t callback)
{
      m_custom_command_callback = callback;
}


void ad_set_debug_field(uint8_t index, uint16_t field_value)
{
    uint32_t i = m_head;

    // Check if entry already exists, if not add it to the queue.
    if(m_hash_lookup[index] != ANT_DEBUG_FIELD_INVALID)
    {
        i = m_hash_lookup[index];
    }
    else
    {
        m_hash_lookup[index]        = m_head;
        m_debug_queue[m_head].index = index;

        m_head++;
        m_queue_size++;
    }

    m_debug_queue[i].value = field_value;
}


bool ad_get_debug_field(uint8_t index, uint16_t* p_field_value)
{
    debug_value_t value = m_debug_queue[m_hash_lookup[index]];

    if(value.index != ANT_DEBUG_FIELD_INVALID)
    {
        *p_field_value = value.value;
        return true;
    }
    return false;
}


void ad_increment_debug_field(uint8_t index)
{
    uint16_t value;
    if(ad_get_debug_field(index, &value))
    {
        value++;
        ad_set_debug_field(index, value);
    }
}


void ad_set_fast_debug_byte(uint8_t fdb_value)
{
   m_fast_debug_byte = fdb_value;
}


void ad_force_error_page(uint8_t error_code, uint16_t error_line, const char* file_name)
{
    m_tx_buffer[0] = ANT_DEBUG_PAGE_MESSAGE;
    m_tx_buffer[1] = error_code;
    m_tx_buffer[2] = ANT_DEBUG_FIELD_FILE_NAME;
    m_tx_buffer[3] = (uint8_t) file_name[1];
    m_tx_buffer[4] = (uint8_t) file_name[0];
    m_tx_buffer[5] = ANT_DEBUG_FIELD_ERR_LINE;
    m_tx_buffer[6] = (uint8_t)(error_line >> 0);
    m_tx_buffer[7] = (uint8_t)(error_line >> 8);
    UNUSED_VARIABLE(sd_ant_broadcast_message_tx(DEBUG_CHANNEL, ANT_STANDARD_DATA_PAYLOAD_SIZE, m_tx_buffer));
}


void ad_process_ant_event(ant_evt_t * p_ant_evt)
{
    switch (p_ant_evt->event)
    {
        case EVENT_CHANNEL_COLLISION:   // Intentional fall through
        case EVENT_TRANSFER_TX_FAILED:  // Intentional fall through
        case EVENT_TX:
            ad_update_tx();
            break;

        case EVENT_RX:
            switch(p_ant_evt->msg.evt_buffer[3])
            {
                case ANT_DEBUG_PAGE_MESSAGE: // Filter message
                    ad_decode_debug_command(p_ant_evt);
                    break;

                default: // Custom debug command
                    if(m_custom_command_callback != NULL) // Check if callback has been registered
                    {
                        m_custom_command_callback(&p_ant_evt->msg.evt_buffer[3]);
                    }
                    break;
            }
            break;

        default:
            break;
    }
}



#endif



