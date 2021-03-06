/*!
 * \file
 * \brief 
 * \author Joanna,,,
 */

#ifndef MLS_HPP_
#define MLS_HPP_

#include "Component_Aux.hpp"
#include "Component.hpp"
#include "DataStream.hpp"
#include "Property.hpp"
#include "EventHandler2.hpp"

#include <pcl/point_types.h>
#include <pcl/point_cloud.h>

namespace Processors {
namespace MLS {

/*!
 * \class MLS
 * \brief MLS processor class.
 *
 * MLS processor.
 */
class MLS: public Base::Component {
public:
	/*!
	 * Constructor.
	 */
	MLS(const std::string & name = "MLS");

	/*!
	 * Destructor
	 */
	virtual ~MLS();

	/*!
	 * Prepare components interface (register streams and handlers).
	 * At this point, all properties are already initialized and loaded to 
	 * values set in config file.
	 */
	void prepareInterface();

protected:

	/*!
	 * Connects source to given device.
	 */
	bool onInit();

	/*!
	 * Disconnect source from device, closes streams, etc.
	 */
	bool onFinish();

	/*!
	 * Start component
	 */
	bool onStart();

	/*!
	 * Stop component
	 */
	bool onStop();

	Base::DataStreamIn<pcl::PointCloud<pcl::PointXYZRGB>::Ptr> in_cloud_xyzrgb;
	Base::DataStreamOut<pcl::PointCloud<pcl::PointXYZRGB>::Ptr> out_cloud_xyzrgb;

	// Handlers
	Base::EventHandler2 h_compute_xyzrgb;

	Base::Property<double> radius_search;

	void compute();

};

} //: namespace MLS
} //: namespace Processors

/*
 * Register processor component.
 */
REGISTER_COMPONENT("MLS", Processors::MLS::MLS)

#endif /* MLS_HPP_ */
