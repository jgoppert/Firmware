#include "Cei.hpp"
#include "gen/att_lgpekf/casadi_att_lgpekf.h"
#include <cstdlib>
#include <drivers/drv_hrt.h>
#include <string.h>
#include <assert.h>


Cei::Cei() :
	SuperBlock(nullptr, "CEI"),
	ModuleParams(nullptr),
	_perf_update(),
	_perf_predict(),
	_perf_mag(),
	_perf_accel(),
	_perf_publish(),
	_perf_subscribe(),
	_initialized(false),
	_shadow(false),

	// blocks
	_mag_stats(this, ""),
	_accel_stats(this, ""),

	// casadi functions
	_mrp_shadow(mrp_shadow_functions()),
	_mrp_to_quat(mrp_to_quat_functions()),
	_quat_to_euler(quat_to_euler_functions()),
	_predict(predict_x_W_functions()),
	_correct_accel(correct_accel_functions()),
	_correct_mag(correct_mag_functions()),
	_init(init_functions()),

	// subscriptions
	_sub_param_update(ORB_ID(parameter_update), 1000 / 2, 0),
	_sub_sensor(ORB_ID(sensor_combined), 1000 / 200, 0),
	_sub_mag(ORB_ID(vehicle_magnetometer), 1000 / 50, 0),

	// publications
	_pub_att(ORB_ID(vehicle_attitude)),
	_pub_est(ORB_ID(estimator_status)),
	_pub_innov(ORB_ID(ekf2_innovations)),

	// misc
	_polls()
{
	// counters
	_perf_update = perf_alloc(PC_ELAPSED, "cei_update");
	_perf_predict = perf_alloc(PC_ELAPSED, "cei_predict");
	_perf_mag = perf_alloc(PC_ELAPSED, "cei_mag");
	_perf_accel = perf_alloc(PC_ELAPSED, "cei_accel");
	_perf_publish = perf_alloc(PC_ELAPSED, "cei_publish");
	_perf_subscribe = perf_alloc(PC_ELAPSED, "cei_subscribe");

	_polls[POLL_PARAM].fd = _sub_param_update.getHandle();
	_polls[POLL_PARAM].events = POLLIN;

	_polls[POLL_SENSORS].fd = _sub_sensor.getHandle();
	_polls[POLL_SENSORS].events = POLLIN;

	_x.setZero();
	_W.setZero();
	status();
}

Cei::~Cei()
{
	perf_free(_perf_update);
	perf_free(_perf_predict);
	perf_free(_perf_mag);
	perf_free(_perf_accel);
	perf_free(_perf_publish);
	perf_free(_perf_subscribe);
}

void Cei::update()
{

	// wait for a sensor update, check for exit condition every 100 ms
	int ret = px4_poll(_polls, n_poll, 100);

	if (ret < 0) {
		PX4_WARN("poll failed");
		return;
	}

	uint64_t now = hrt_absolute_time();
	float dt = (now - _timeStamp) / 1.0e6f;

	// abort if not enough time elapsed
	if (dt < 1e-3f) {
		return;
	}

	_timeStamp =  now;

	// check for sane update rate
	if (dt > 0.1f) {
		PX4_WARN("update rate slow: %12.5f", double(dt));
		return;
	}

	perf_begin(_perf_update);

	// set dt for all child blocks
	setDt(dt);

	perf_begin(_perf_subscribe);

	// get updates
	bool mag_updated = _sub_mag.updated();
	bool accel_updated = _sub_sensor.updated();

	// update params
	if (_sub_param_update.updated()) {
		ModuleParams::updateParams();
		SuperBlock::updateParams();
	}

	// update all subsription data
	updateSubscriptions();

	// gyro reading
	Vector3f omega_b(_sub_sensor.get().gyro_rad);

	perf_end(_perf_subscribe);

	// predict
	if (!_initialized) {

		_mag_stats.update(Vector3f(_sub_mag.get().magnetometer_ga));
		_accel_stats.update(Vector3f(_sub_sensor.get().accelerometer_m_s2));

		if (_mag_stats.getCount() > 10 and _accel_stats.getCount() > 10) {

			// init:(g_b[3],B_b[3],decl,std_x0[6])->(x0[6],W0[6x6,21nz],error_code)
			float init_ret = 0;
			float x1[6] = {0};
			float W1[n_W] = {0};
			float std0[6] = {1, 1, 1, 0.1, 0.1, 0.1};
			float decl = _decl.get();
			Vector3f y_accel = _accel_stats.getMean();
			Vector3f y_mag = _mag_stats.getMean();

			_init.arg(0, y_accel.data());
			_init.arg(1, y_mag.data());
			_init.arg(2, &decl);
			_init.arg(3, std0);
			_init.res(0, x1);
			_init.res(1, W1);
			_init.res(2, &init_ret);
			_init.eval();
			int init_flag = int(init_ret);

			if (init_flag == 0) {
				_initialized = true;
				PX4_INFO("initialized");
				handle_correction(x1, W1, init_ret, 2, OUTLIER_FUSE, "init");

			} else {
				PX4_INFO("initialization failed: %d", init_flag);
				_accel_stats.getMean().print();
				_mag_stats.getMean().print();
				Vector<float, 6>(x1).print();
			}
		}

	} else {

		// temporary variables for correction/prediction return
		float x1[n_x] = {0};
		float W1[n_W] = {0};

		// prediction
		{
			/* predict_x_W:(x0[6],W0[6x6,21nz],omega_b[3],std_gyro,sn_gyro_rw,dt)->(x1[6],W1[6x6,21nz]) */
			perf_begin(_perf_predict);
			float std_gyro = 1e-3f * _std_gyro.get();
			float sn_gyro_rw = 1e-3f * _sn_gyro_rw.get();
			_predict.arg(0, _x.data());
			_predict.arg(1, _W.data());
			_predict.arg(2, omega_b.data());
			_predict.arg(3, &std_gyro);
			_predict.arg(4, &sn_gyro_rw);
			_predict.arg(5, &dt);
			_predict.res(0, x1);
			_predict.res(1, W1);
			_predict.eval();
			handle_correction(x1, W1, 0, 2, OUTLIER_FUSE, "predict");
			perf_end(_perf_predict);
		}

		// correct mag
		if (mag_updated) {
			// correct_mag:(x_h[6],W[6x6,21nz],y_b[3],decl,std_mag,beta_mag_c)->
			// (x_mag[6],W_mag[6x6,21nz],beta_mag,r_mag,r_std_mag,error_code)
			perf_begin(_perf_mag);
			const float *y_b = _sub_mag.get().magnetometer_ga;
			float decl = _decl.get();
			float std_mag = 1e-3f * _std_mag.get();
			float beta_mag_c = _beta_mag_c.get();
			float beta_mag = 0;
			float r_mag = 0;
			float r_std_mag = 0;
			float mag_ret = 0;
			_correct_mag.arg(0, _x.data());
			_correct_mag.arg(1, _W.data());
			_correct_mag.arg(2, y_b);
			_correct_mag.arg(3, &decl);
			_correct_mag.arg(4, &std_mag);
			_correct_mag.arg(5, &beta_mag_c);
			_correct_mag.res(0, x1);
			_correct_mag.res(1, W1);
			_correct_mag.res(2, &beta_mag);
			_correct_mag.res(3, &r_mag);
			_correct_mag.res(4, &r_std_mag);
			_correct_mag.res(5, &mag_ret);
			_correct_mag.eval();
			handle_correction(x1, W1, mag_ret, beta_mag, OUTLIER_FUSE, "mag");
			_pub_est.get().mag_test_ratio = beta_mag;
			_pub_innov.get().mag_innov[0] = r_mag;
			_pub_innov.get().mag_innov_var[0] = r_std_mag * r_std_mag;
			perf_end(_perf_mag);
		}

		// correct accel
		if (accel_updated) {
			// correct_accel:(x_h[6],W[6x6,21nz],y_b[3],omega_b[3],std_accel,std_accel_omega,beta_accel_c)->
			// (x_accel[6],W_accel[6x6,21nz],beta_accel,r_accel[2],r_std_accel[2],error_code)
			perf_begin(_perf_accel);
			const float *y_b = _sub_sensor.get().accelerometer_m_s2;
			float std_acc = 1e-3f * _std_acc.get();
			float std_acc_w = 1e-3f * _std_acc_w.get();
			float beta_acc_c = _beta_acc_c.get();
			float beta_acc = 0;
			float r_acc[2] = {0};
			float r_std_acc[2] = {0};
			float accel_ret = 0;
			_correct_accel.arg(0, _x.data());
			_correct_accel.arg(1, _W.data());
			_correct_accel.arg(2, y_b);
			_correct_accel.arg(3, omega_b.data());
			_correct_accel.arg(4, &std_acc);
			_correct_accel.arg(5, &std_acc_w);
			_correct_accel.arg(6, &beta_acc_c);
			_correct_accel.res(0, x1);
			_correct_accel.res(1, W1);
			_correct_accel.res(2, &beta_acc);
			_correct_accel.res(3, r_acc);
			_correct_accel.res(4, r_std_acc);
			_correct_accel.res(5, &accel_ret);
			_correct_accel.eval();
			handle_correction(x1, W1, accel_ret, beta_acc, OUTLIER_FUSE, "accel");
			perf_end(_perf_accel);
		}
	}

	perf_begin(_perf_publish);

	// publish vehicle_attitude
	if (true) {
		Quatf q = compute_quaternion();
		vehicle_attitude_s &att = _pub_att.get();
		att.timestamp = now;

		for (int i = 0; i < 4; i++) {
			att.delta_q_reset[i] = 0;
			att.q[i] = q(i);
		}

		att.quat_reset_counter = 0;

		//att.rollspeed = omega_b(0) - _x(X_bgx);
		//att.pitchspeed = omega_b(1) - _x(X_bgy);
		//att.yawspeed = omega_b(2) - _x(X_bgz);
		// TODO publish sensor bias
		_pub_att.update();
	}

	// publish estimator status
	if (true) {
		estimator_status_s &est = _pub_est.get();

		for (int i = 0; i < 24; i++) {
			if (i < n_W) {
				est.covariances[i] = _W(i);

			} else {
				est.covariances[i] = 0;
			}
		}

		est.n_states = 6;

		for (int i = 0; i < 24; i++) {
			if (i < n_x) {
				est.states[i] = _x(i);

			} else {
				est.states[i] = 0;
			}
		}

		_pub_est.update();
	}

	// innovations
	if (false) {
		_pub_innov.update();
	}

	perf_end(_perf_publish);

	perf_end(_perf_update);
}

void Cei::status()
{
	PX4_INFO("initialized: %d", _initialized);

	for (int i = 0; i < 6; i++) {
		PX4_INFO("x(%2d) = %20.10f", i, double(_x(i)));
	}

	for (int i = 0; i < 21; i++) {
		PX4_INFO("W(%2d) = %20.10f", i, double(_W(i)));
	}

	perf_print_counter(_perf_update);
	perf_print_counter(_perf_predict);
	perf_print_counter(_perf_mag);
	perf_print_counter(_perf_accel);
	perf_print_counter(_perf_publish);
	perf_print_counter(_perf_subscribe);
}


bool Cei::array_finite(float *a, int n)
{
	for (int i = 0; i < n; i++) {
		if (!PX4_ISFINITE(a[i])) {
			return false;
		}
	}

	return true;
}

void Cei::handle_correction(float *x, float *W, float ret,
			    float beta, outlier_action_t action, const char *msg)
{
	int error_code = int(ret);

	if (error_code != 0) {
		return;

	} else if (!array_finite(W, n_W)) {
		PX4_WARN("%s, non finite covariance", msg);

	} else if (!array_finite(x, n_x)) {
		PX4_WARN("%s, non finite correction state", msg);

	} else if (error_code == 0 && (beta < 1 || action == OUTLIER_FUSE)) {
		memcpy(_W.data(), W, sizeof(float)*n_W);
		memcpy(_x.data(), x, sizeof(float)*n_x);
		handle_shadow();
	}
}

Quatf Cei::compute_quaternion()
{
	/* mrp_to_quat:(r[3])->(q[4]) */
	Vector3f r(_x.slice<3, 1>(0, 0));
	Quatf q;
	_mrp_to_quat.arg(0, r.data());
	_mrp_to_quat.res(0, q.data());
	_mrp_to_quat.eval();

	// handle mrp shadow for consistent quaternion
	// output
	if (_shadow) {
		q = -q;
	}

	return q;
}

void Cei::handle_shadow()
{
	/* mrp_shadow:(r[3])->(r_s[3]) */
	Vector3f r(_x.slice<3, 1>(0, 0));

	if (r.norm() > 1) {
		Vector3f r_s;
		_mrp_shadow.arg(0, r.data());
		_mrp_shadow.res(0, r_s.data());
		_mrp_shadow.eval();
		_x(X_rx) = r_s(0);
		_x(X_ry) = r_s(1);
		_x(X_rz) = r_s(2);
		_shadow = !_shadow;
	}
}
