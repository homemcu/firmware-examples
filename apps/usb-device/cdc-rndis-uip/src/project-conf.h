/*
* Copyright (c) 2019 Vladimir Alemasov
* All rights reserved
*
* This program and the accompanying materials are distributed under 
* the terms of GNU General Public License version 2 
* as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/

#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

#ifdef USBD_OTGFS
#define USBD_FULL_SPEED
#define USBD_VBUS_DETECT
#elif defined USBD_OTGHS_FS
#define USBD_FULL_SPEED
#elif defined USBD_OTGHS
#endif
#define USBD_SOF_DISABLED

#endif /* PROJECT_CONF_H_ */