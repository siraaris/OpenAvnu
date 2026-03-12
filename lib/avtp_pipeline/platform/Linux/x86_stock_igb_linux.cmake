# generic Linux build settings for stock igb + socket-based launch-time TX

set ( AVB_FEATURE_IGB 0 )
set ( IGB_LAUNCHTIME_ENABLED 0 )
set ( SOCKET_LAUNCHTIME_ENABLED 1 )
set ( AVB_FEATURE_PCAP 1 )

# Label for messages / build configuration
set ( OPENAVB_HAL      "generic" )
set ( OPENAVB_OSAL     "Linux" )
set ( OPENAVB_TCAL     "GNU" )
set ( OPENAVB_PLATFORM "${OPENAVB_HAL}-${OPENAVB_OSAL}" )

set ( PLATFORM_INCLUDE_DIRECTORIES
	${CMAKE_SOURCE_DIR}/openavb_common
	${CMAKE_SOURCE_DIR}/../../daemons/common
	${CMAKE_SOURCE_DIR}/../../daemons/mrpd
	${CMAKE_SOURCE_DIR}/../../daemons/maap/common
)

include_directories ( platform/generic/include )

set ( GSTREAMER_1_0 1 )
