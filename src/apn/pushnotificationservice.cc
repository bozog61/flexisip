/*
 Flexisip, a flexible SIP proxy server with media capabilities.
 Copyright (C) 2012  Belledonne Communications SARL.

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU Affero General Public License as
 published by the Free Software Foundation, either version 3 of the
 License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU Affero General Public License for more details.

 You should have received a copy of the GNU Affero General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <sys/types.h>
#include <dirent.h>

#include "pushnotificationservice.h"
#include "pushnotificationclient.h"
#include "common.hh"

#include <boost/bind.hpp>
#include <sstream>

const char *APN_ADDRESS = "gateway.sandbox.push.apple.com";
const char *APN_PORT = "2195";

using namespace ::std;
using namespace ::boost;

void PushNotificationService::sendRequest(const std::shared_ptr<PushNotificationRequest> &pn) {
	std::shared_ptr<PushNotificationClient> client=mClients[pn->getAppIdentifier()+string(".pem")];
	if (client==0){
		LOGE("No push notification certificate for client %s",pn->getAppIdentifier().c_str());
		return;
	}
	
	if (client->isReady()) {
		client->send(pn->getData());
		return;
	}
	// Wait for free thread
	LOGE("Client is not ready ! push notification is lost.");
		
}

void PushNotificationService::start() {
	if (mThread == NULL || !mThread->joinable()) {
		delete mThread;
		mThread = NULL;
	}
	if (mThread == NULL) {
		LOGD("Start PushNotificationService");
		mHaveToStop = false;
		mThread = new thread(&PushNotificationService::run, this);
	}
}

void PushNotificationService::stop() {
	if (mThread != NULL) {
		LOGD("Stopping PushNotificationService");
		mHaveToStop = true;
		mIOService.stop();
		if (mThread->joinable()) {
			mThread->join();
		}
		delete mThread;
		mThread = NULL;
	}
}

void PushNotificationService::setupClients(const string &certdir, const string &ca){
	struct dirent *dirent;
	DIR *dirp;
	
	dirp=opendir(certdir.c_str());
	if (dirp==NULL){
		LOGE("Could not open push notification certificates directory (%s): %s",certdir.c_str(),strerror(errno));
		return;
	}
	while((dirent=readdir(dirp))!=NULL){
		if (dirent->d_type!=DT_REG && dirent->d_type!=DT_LNK) continue;
		string cert=string(dirent->d_name);
		string certpath= string(certdir)+"/"+cert;
		std::shared_ptr<boost::asio::ssl::context> ctx(new boost::asio::ssl::context(mIOService, asio::ssl::context::sslv23_client));
		system::error_code err;
		ctx->set_options(asio::ssl::context::default_workarounds, err);
		ctx->set_password_callback(bind(&PushNotificationService::handle_password_callback, this, _1, _2));

		if (!ca.empty()) {
			ctx->set_verify_mode(asio::ssl::context::verify_peer);
	#if BOOST_VERSION >= 104800
			ctx->set_verify_callback(bind(&PushNotificationService::handle_verify_callback, this, _1, _2));
	#endif
			ctx->load_verify_file(ca, err);
			if (err) {
				LOGE("load_verify_file: %s",err.message().c_str());
			}
		} else {
			ctx->set_verify_mode(asio::ssl::context::verify_none);
		}
		ctx->add_verify_path("/etc/ssl/certs");

		if (!cert.empty()) {
			ctx->use_certificate_file(certpath, asio::ssl::context::file_format::pem, err);
			if (err) {
				LOGE("use_certificate_file %s: %s",certpath.c_str(), err.message().c_str());
			}
		}
		string key=certpath;
		if (!key.empty()) {
			ctx->use_private_key_file(key, asio::ssl::context::file_format::pem, err);
			if (err) {
				LOGE("use_private_key_file: %s",err.message().c_str());
			}
		}
		mClients[cert]=make_shared<PushNotificationClient>(cert, this,ctx,APN_ADDRESS,APN_PORT);
	}
	closedir(dirp);
}

PushNotificationService::PushNotificationService(const std::string &certdir, const std::string &cafile) :
		mIOService(), mThread(NULL) {
	setupClients(certdir,cafile);
}

PushNotificationService::~PushNotificationService() {
	stop();
}

int PushNotificationService::run() {
	LOGD("PushNotificationService Start");
	asio::io_service::work work(mIOService);
	mIOService.run();
	LOGD("PushNotificationService End");
	return 0;
}

void PushNotificationService::clientEnded() {
	unique_lock<mutex> lock(mMutex);
	mClientCondition.notify_all();
}

asio::io_service &PushNotificationService::getService() {
	return mIOService;
}

string PushNotificationService::handle_password_callback(size_t max_length, asio::ssl::context_base::password_purpose purpose) const {
	return mPassword;
}

#if BOOST_VERSION >= 104800
bool PushNotificationService::handle_verify_callback(bool preverified, asio::ssl::verify_context& ctx) const {
	if (IS_LOGD) {
		char subject_name[256];
		X509* cert = X509_STORE_CTX_get_current_cert(ctx.native_handle());
		X509_NAME_oneline(X509_get_subject_name(cert), subject_name, 256);
		LOGD("Verifying %s", subject_name);
	}
	return preverified;
}
#endif