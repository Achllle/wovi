
#include "defs.h"
#include "sphere_grid.hpp"
#include <sensor_msgs/Joy.h>
#include <sensor_msgs/JointState.h>
#include <keyboard/Key.h>
#include <view_controller_msgs/CameraPlacement.h>
#include <tf/transform_broadcaster.h>
#include <sstream>

#include "lwrlib/LwrLibrary.hpp"
#include "helpers.h"

#include "IKRay.h"

  ///////////////////
 // main function //
///////////////////

void sonar(){

    ////////////////////////////// create publishers and subscribers

    ros::NodeHandle node;
    ros::Subscriber subjoy = node.subscribe("joy", 5, joyCallback);
    ros::Subscriber subkeydn = node.subscribe("keyboard/keydown", 5, keydownCallback);
    ros::Subscriber subkeyup = node.subscribe("keyboard/keyup", 5, keyupCallback);
    for(int i=0; i<1024; i++){keyState[i] = false;}

    double dt = 1.0/10.0;//1.0/10.0;
    double t = 0;
    ros::Rate rate(1.0/dt);
    ros::Publisher publisherWorkspace = node.advertise <visualization_msgs::Marker> ("workspace", 1);
    ros::Publisher publisherOriWorkspace = node.advertise <visualization_msgs::Marker> ("oriworkspace", 5);
    ros::Publisher publisherRobot = node.advertise <visualization_msgs::Marker> ("robot", 1);
    ros::Publisher publisherTool = node.advertise <visualization_msgs::Marker> ("tool", 1);
    ros::Publisher publisherJointLabels = node.advertise <visualization_msgs::Marker> ("jointlabels", 7);
    ros::Publisher publisherElbowCost = node.advertise <visualization_msgs::Marker> ("elbowcost", 2);
    ros::Publisher publisherCam = node.advertise <view_controller_msgs::CameraPlacement>("rviz/camera_placement", 1);
    ros::Publisher publisherModel = node.advertise <sensor_msgs::JointState> ("joint_states", 1);

    bool autoElbow = true;

    ////////////////////////////// initialize markers for sonar (ik probe rays)

    bool sonarUseTriangles = true;

    visualization_msgs::Marker markerSonar;

    int splitFactor = 10;
    int nSonarRays = sphere_icos_point_num(splitFactor);

    // geodesic sphere

    printf("\n\ncreating geodesic sphere for probe directions using qconvex (qhull must be installed!)...\n");

    double *sphereGridPoints = new double[nSonarRays*3];
    sphereGridPoints = sphere_icos2_points(splitFactor, nSonarRays);

    // calculate faces using external program qhull (qconvex)

    stringstream ss;
    ss << "echo \"3 " << nSonarRays << "\n";

    for(int i=0; i<nSonarRays; i++){
        ss << sphereGridPoints[i*3  ] << " ";
        ss << sphereGridPoints[i*3+1] << " ";
        ss << sphereGridPoints[i*3+2] << "\n";
    }
    ss << "\" | qconvex i";

    //cout << "\n\n" << ss.str() << "\n\n";

    FILE *fp;

    fp = popen(ss.str().c_str(), "r");
    if (fp == NULL){printf("Failed to run command\n" ); exit(1);}

    int nFaces;
    fscanf(fp, "%d", &nFaces);
    vector<int> geoFaces(nFaces*3, -1);
    int i1, i2, i3;
    for(int i=0; i<nFaces; i++){
        fscanf(fp, "%d %d %d\n", &i1, &i2, &i3);
        geoFaces[i*3  ] = i1;
        geoFaces[i*3+1] = i2;
        geoFaces[i*3+2] = i3;
    }
    pclose(fp);

    if(!sonarUseTriangles){
        markerSonar = initializeMarker(0, visualization_msgs::Marker::POINTS, nSonarRays);
        markerSonar.scale.x = markerSonar.scale.y = markerSonar.scale.z = 0.01;
    }
    else{
        markerSonar = initializeMarker(0, visualization_msgs::Marker::TRIANGLE_LIST, nFaces*3);
    }

    ////////////////////////////// initialize line marker for robot and joints for publishing

    visualization_msgs::Marker markerRobot =
            initializeMarker(1, visualization_msgs::Marker::LINE_STRIP, 7);
    markerRobot.scale.x = markerRobot.scale.y = markerRobot.scale.z = 0.04;

    visualization_msgs::Marker markerTool = initializeMarker(37, visualization_msgs::Marker::MESH_RESOURCE, 0);
    markerTool.mesh_resource = "file:///home/mirko/catkin_ws/src/wovi/misc/rubiktool.dae";
    markerTool.mesh_use_embedded_materials = true;
    markerTool.color = colorFromEigen(Vector4d(0.1,0.3,1,1));

    sensor_msgs::JointState jointsPublish;
    jointsPublish.position.resize(7);
    jointsPublish.name.resize(7);
    jointsPublish.name[0] = "lwr1_base_joint";
    jointsPublish.name[1] = "lwr1_shoulder_joint";
    jointsPublish.name[2] = "lwr1_upperarm_joint";
    jointsPublish.name[3] = "lwr1_elbow_joint";
    jointsPublish.name[4] = "lwr1_lowerarm_joint";
    jointsPublish.name[5] = "lwr1_wrist_joint";
    jointsPublish.name[6] = "lwr1_flange_joint";

    ////////////////////////////// initialize text markers for robot joints

    visualization_msgs::Marker markerJoints[7];
    for(int j=0; j<7; j++){
        markerJoints[j] = initializeMarker(10+j, visualization_msgs::Marker::TEXT_VIEW_FACING, 1);
        markerJoints[j].scale.x = markerJoints[j].scale.y = markerJoints[j].scale.z = 0.04;
        markerJoints[j].color = colorFromEigen(Vector4d(0,0,0.8,1));
    }

    ////////////////////////////// initialize triangle marker for elbow cost function

    int nElbowCostSteps = 256;
    double elbowCostR1 = 0.5;
    double elbowCostR2 = 0.6;

    visualization_msgs::Marker markerElbowCost =
            initializeMarker(2, visualization_msgs::Marker::LINE_STRIP, nElbowCostSteps+1);
    markerElbowCost.scale.x = markerElbowCost.scale.y = markerElbowCost.scale.z = 0.02;

    visualization_msgs::Marker markerElbowZero =
            initializeMarker(3, visualization_msgs::Marker::LINE_STRIP, nElbowCostSteps+1);
    markerElbowZero.scale.x = markerElbowZero.scale.y = markerElbowZero.scale.z = 0.004;

    for(int iv=0; iv<nElbowCostSteps+1; iv++){
        markerElbowZero.colors[iv].r = 0.8; markerElbowZero.colors[iv].g = 0.9; markerElbowZero.colors[iv].b = 1.0;
    }

    ////////////////////////////// initialize orientation targets and line marker for orientation checks

    int nOriSonarRays = 128+2; // tilting about 32 axes and rolling to the left and to the right
    Quaterniond *targetOrisRel = new Quaterniond[nOriSonarRays];

    double oriPhiMax = 0.5*M_PI;

    for(int i=0; i<nOriSonarRays-2; i++){
        double yaw = (double)i / ((double)nOriSonarRays-2) *2.0*M_PI;
        targetOrisRel[i] = AngleAxisd(oriPhiMax, Vector3d(cos(yaw), sin(yaw), 0) );
    }

    targetOrisRel[nOriSonarRays-2] = AngleAxisd( -oriPhiMax, Vector3d(0,0,1) );
    targetOrisRel[nOriSonarRays-1] = AngleAxisd( +oriPhiMax, Vector3d(0,0,1) );

    visualization_msgs::Marker markerOriSonar =
            initializeMarker(4, visualization_msgs::Marker::LINE_STRIP, nOriSonarRays-2+1);
    markerOriSonar.scale.x = markerOriSonar.scale.y = markerOriSonar.scale.z = 0.01;
    markerOriSonar.color.a = 0.99; // so per vertex alpha is active

    visualization_msgs::Marker markerRollSonar =
            initializeMarker(5, visualization_msgs::Marker::LINE_LIST, 6);
    markerRollSonar.scale.x = markerRollSonar.scale.y = markerRollSonar.scale.z = 0.01;
    markerRollSonar.color.a = 0.99; // so per vertex alpha is active

    visualization_msgs::Marker markerOriSonarSphere =
            initializeMarker(6, visualization_msgs::Marker::SPHERE, 1);
    markerOriSonarSphere.scale.x = markerOriSonarSphere.scale.y = markerOriSonarSphere.scale.z = 0.5;

    markerOriSonarSphere.pose.position.z = 0.31;
    markerOriSonarSphere.color = colorFromEigen(Vector4d(1,1,1,0.5));
    markerOriSonarSphere.pose.orientation = orientationFromEigen(Vector4d(0.5,0.5,0.5,0.5));


    ////////////////////////////// configure camera position

    view_controller_msgs::CameraPlacement cam;
    double camyaw = 0;
    double campitch = 0;
    double camdst = 3;
    cam.interpolation_mode = 1;
    cam.target_frame = "";
    cam.time_from_start.sec = 0;
    cam.time_from_start.nsec = 1;//20*1e6;
    cam.eye.point.x = 3; cam.eye.point.y = 2; cam.eye.point.z = 1;
    cam.focus.point.x = 0; cam.focus.point.y = 0; cam.focus.point.z = 0.31;
    cam.up.vector.x = 0; cam.up.vector.y = 0; cam.up.vector.z = 1;
    cam.mouse_interaction_mode = 0;
    cam.interaction_disabled = false;
    cam.allow_free_yaw_axis = false;

    ////////////////////////////// prepare GPU stuff

    printf("compiling for GPU...\n");
    IKRay ikray;

    printf("\n\nfilling arrays...\n");
    int nSonarRays64 = ((nSonarRays+nOriSonarRays)/64+1)*64; // increased so 64 devides it

    FLOAT* endParams = new FLOAT[nSonarRays64];
    FLOAT* rayEnds = new FLOAT[nSonarRays64*4]; // cl_double3 is the same as cl_double4
    FLOAT* rayColors = new FLOAT[nSonarRays64*4];

    FLOAT* tcpPosStart = new FLOAT[4];
    FLOAT* tcpOriStart = new FLOAT[4];

    LwrFrame tool;
    tool.setPos(0,0,0.3);
    tool.setQuat(1,0,0,0);

    LwrFrame invTool = tool.inverse();

    FLOAT* invToolOcl = new FLOAT[16];

    invToolOcl[0]=invTool.ori(0,0); invToolOcl[4]=invTool.ori(0,1); invToolOcl[ 8]=invTool.ori(0,2); invToolOcl[12]=invTool.pos(0);
    invToolOcl[1]=invTool.ori(1,0); invToolOcl[5]=invTool.ori(1,1); invToolOcl[ 9]=invTool.ori(1,2); invToolOcl[13]=invTool.pos(1);
    invToolOcl[2]=invTool.ori(2,0); invToolOcl[6]=invTool.ori(2,1); invToolOcl[10]=invTool.ori(2,2); invToolOcl[14]=invTool.pos(2);
    invToolOcl[3]=0;                invToolOcl[7]=0;                invToolOcl[11]=0;                invToolOcl[15]=1;


    LwrFrame currentTcp;
    LwrXCart currentPose;

    currentTcp.setQuat(0.0, 0.0, 1.0, 0.0);
    currentTcp.setPos(-0.6,0.0,0.3);

    currentPose.pose = currentTcp*invTool;
    tcpPosStart[0]=currentTcp.pos.x(); tcpPosStart[1]=currentTcp.pos.y(); tcpPosStart[2]=currentTcp.pos.z();
    double buffer4[4];
    currentTcp.getQuat(buffer4); // w x y z
    tcpOriStart[0]=buffer4[0]; tcpOriStart[1]=buffer4[1]; tcpOriStart[2]=buffer4[2]; tcpOriStart[3]=buffer4[3];

    FLOAT nsparam = 0;
    int8_t config = 2;
    FLOAT* targets = new FLOAT[nSonarRays64*4];
    FLOAT* targetOris = new FLOAT[nSonarRays64*4];
    uint32_t nrays = nSonarRays64;
    uint32_t nsteps = 25;
    FLOAT probeRadius = 0.7;

    bool doublePrecision = false;
    #ifdef DOUBLE_PRECISION
        doublePrecision = true;
    #endif

    for(int iv=0; iv<nSonarRays; iv++){
        targets[iv*4  ] = tcpPosStart[0] + sphereGridPoints[iv*3  ]*probeRadius;
        targets[iv*4+1] = tcpPosStart[1] + sphereGridPoints[iv*3+1]*probeRadius;
        targets[iv*4+2] = tcpPosStart[2] + sphereGridPoints[iv*3+2]*probeRadius;

        targetOris[iv*4  ] = tcpOriStart[0];
        targetOris[iv*4+1] = tcpOriStart[1];
        targetOris[iv*4+2] = tcpOriStart[2];
        targetOris[iv*4+3] = tcpOriStart[3];
    }

    for(int iv=0; iv<nOriSonarRays; iv++){
        targets[(iv+nSonarRays)*4  ] = tcpPosStart[0];
        targets[(iv+nSonarRays)*4+1] = tcpPosStart[1];
        targets[(iv+nSonarRays)*4+2] = tcpPosStart[2];

        Quaterniond bufq(tcpOriStart[0], tcpOriStart[1], tcpOriStart[2], tcpOriStart[3]);
        bufq = bufq * targetOrisRel[iv];

        targetOris[(iv+nSonarRays)*4  ] = bufq.w();
        targetOris[(iv+nSonarRays)*4+1] = bufq.x();
        targetOris[(iv+nSonarRays)*4+2] = bufq.y();
        targetOris[(iv+nSonarRays)*4+3] = bufq.z();
    }

    printf("GPUing...\n");
    tic();
    ikray.compute(
                endParams,
                rayEnds,
                rayColors,
                tcpPosStart,
                tcpOriStart,
                invToolOcl,
                &nsparam,
                config,
                targets,
                targetOris,
                nrays,
                nsteps,
                true,
                doublePrecision);
    toc();
    printf("done.\n");



    ////////////////////////////// game loop

    bool firstRun = true;
    while (ros::ok())
    {

        ////////////////////////////// gamepad control

        /*
        camyaw += -0.2*sonarJoyAxesState[3];
        campitch += 0.2*sonarJoyAxesState[4];
        if(campitch > 1.57){campitch = 1.57;}
        if(campitch < -1.57){campitch = -1.57;}
        //camdst *= 1 + 0.1*(sonarJoyButtonsState[4]-sonarJoyButtonsState[5]);

        if(sonarJoyButtonsState[9]){autoElbow = true;}

        if(sonarJoyButtonsState[4] || sonarJoyButtonsState[5]){
            autoElbow = false;
            nsparam += (sonarJoyButtonsState[4]-sonarJoyButtonsState[5])*0.04;
        }

        camdst *= 1 + 0.1*(sonarJoyButtonsState[0]-sonarJoyButtonsState[3]);

        cam.eye.point.x = cos(camyaw)*cos(campitch)*camdst + cam.focus.point.x;
        cam.eye.point.y = sin(camyaw)*cos(campitch)*camdst + cam.focus.point.y;
        cam.eye.point.z = sin(campitch)*camdst + cam.focus.point.z;

        Matrix3d ccs = camcsys(camyaw, campitch, true);

        currentTcp.pos = currentTcp.pos
                + ccs.col(0) * (-0.02*sonarJoyAxesState[0])
                + ccs.col(1) * (+0.02*sonarJoyAxesState[1]);

        currentTcp.ori = AngleAxis<double>(0.1 * (sonarJoyAxesState[5]-sonarJoyAxesState[2]),
                ccs.col(2)) * currentTcp.ori;

        Vector3d camfocus(cam.focus.point.x, cam.focus.point.y, cam.focus.point.z);
        camfocus = camfocus + ccs.col(0) * (-0.02*sonarJoyAxesState[6])
                            + ccs.col(1) * (+0.02*sonarJoyAxesState[7]);

        cam.focus.point.x = camfocus.x(); cam.focus.point.y = camfocus.y(); cam.focus.point.z = camfocus.z();
        */

        camyaw += -0.2*joyAxesState[6];
        campitch += 0.2*joyAxesState[7];
        if(campitch > 1.57){campitch = 1.57;}
        if(campitch < -1.57){campitch = -1.57;}
        camdst *= 1 + 0.1*(joyButtonsState[0]-joyButtonsState[3]);

        Matrix3d cc = camcsys(camyaw, campitch, false); // camera coordinates
        Matrix3d ccs = camcsys(camyaw, campitch, true); // camera coordinates, snapped

        cam.focus.point.x = currentTcp.pos.x();
        cam.focus.point.y = currentTcp.pos.y();
        cam.focus.point.z = currentTcp.pos.z();

        cam.eye.point.x = cos(camyaw)*cos(campitch)*camdst + cam.focus.point.x;
        cam.eye.point.y = sin(camyaw)*cos(campitch)*camdst + cam.focus.point.y;
        cam.eye.point.z = sin(campitch)*camdst + cam.focus.point.z;

        currentTcp.ori = AngleAxis<double>(-0.1 * joyAxesState[3], cc.col(1)) * currentTcp.ori;
        currentTcp.ori = AngleAxis<double>(-0.1 * joyAxesState[4], cc.col(0)) * currentTcp.ori;
        currentTcp.ori = AngleAxis<double>(-0.1 *
             (joyAxesState[5]-joyAxesState[2]), currentTcp.ori.col(2)) * currentTcp.ori;

        currentTcp.pos = currentTcp.pos
                + cc.col(0) * (-0.02*joyAxesState[0])
                + cc.col(1) * (+0.02*joyAxesState[1]);



        tcpPosStart[0]=currentTcp.pos.x(); tcpPosStart[1]=currentTcp.pos.y(); tcpPosStart[2]=currentTcp.pos.z();
        currentTcp.getQuat(buffer4);
        tcpOriStart[0]=buffer4[0]; tcpOriStart[1]=buffer4[1]; tcpOriStart[2]=buffer4[2]; tcpOriStart[3]=buffer4[3];

        ////////////////////////////// elbow optimization

        int nTrials = 15; // how many samples
        double delta = 0.35; // up to which angle (abt. 20deg)
        double nsp = nsparam;

        currentPose.pose = currentTcp*invTool;
        LwrXCart testPose = currentPose;
        LwrJoints currentJoints;
        LwrJoints testJoints;

        testPose.nsparam = nsparam;
        Lwr::inverseKinematics(currentJoints, testPose);
        double costBefore = elbowCost2(currentJoints, currentJoints);

        if(autoElbow){

            testPose.nsparam = nsparam+1e-5;
            Lwr::inverseKinematics(testJoints, testPose);
            double costNext = elbowCost2(testJoints, currentJoints);

            double dir = +1;
            if(costBefore < costNext){dir = -1;}

            // sample cost function in descending direction and stop if it starts rising again

            for(int i=1; i<nTrials+1; i++){
                double q = (double)i/(double)nTrials;
                q = pow(q, 1.5);
                nsp = nsparam + q*dir*delta;

                testPose.nsparam = nsp;
                Lwr::inverseKinematics(testJoints, testPose);
                costNext = elbowCost2(testJoints, currentJoints);

                if(costNext < costBefore){
                    costBefore = costNext;
                    nsparam = nsp;
                }
                else{
                    break;
                }
            }
        }

        ////////////////////////////// draw robot

        currentPose.nsparam = nsparam;
        currentPose.config = config;
        LwrJoints jnts;
        Lwr::inverseKinematics(jnts, currentPose);
        LwrFrame dhPositions[8];
        fk(dhPositions, jnts);
        Vector3d jointPositions[7];

        jointPositions[0] = Vector3d(0,0,0);
        jointPositions[1] = Vector3d(0,0,0.31);
        jointPositions[2] = 0.5*(Vector3d(0,0,0.31) + dhPositions[3].pos);
        jointPositions[3] = dhPositions[3].pos;
        jointPositions[4] = 0.5*(dhPositions[3].pos + dhPositions[5].pos);
        jointPositions[5] = dhPositions[5].pos;
        jointPositions[6] = dhPositions[7].pos;

        for(int j=0; j<7; j++){
            markerRobot.points[j] = positionFromEigen(jointPositions[j]);
            markerRobot.colors[j] = jointColor(j, jnts[j], Vector4d(1.0, 0.8, 0.6, 1.0));
            markerJoints[j].pose.position =
                    positionFromEigen(jointPositions[j]*0.8 + 0.2*Vector3d(cam.eye.point.x, cam.eye.point.y, cam.eye.point.z));
            markerJoints[j].text = int2str((int)(jnts[j]/M_PI*180));
            if(j%2 == 0)
                if(fabs(jnts[j]) > 170.0*M_PI/180.0){
                    markerJoints[j].color = colorFromEigen(Vector4d(0.8,0,0,1));}
                else{
                    markerJoints[j].color = colorFromEigen(Vector4d(0,0.4,1.0,1));}
            else{
                if(fabs(jnts[j]) > 120.0*M_PI/180.0){
                    markerJoints[j].color = colorFromEigen(Vector4d(0.8,0,0,1));}
                else{
                    markerJoints[j].color = colorFromEigen(Vector4d(0,0.4,1.0,1));}
            }
        }



        ////////////////////////////// draw elbow ring

        Vector3d elbowPivot = 0.5*(Vector3d(0,0,0.31) + dhPositions[5].pos);
        Vector3d elbowCircleNormal = dhPositions[5].pos - Vector3d(0,0,0.31);
        elbowCircleNormal.normalize();

        Vector3d sinVec = -elbowCircleNormal.cross(Vector3d(0,0,1));
        sinVec.normalize();
        Vector3d cosVec = -sinVec.cross(elbowCircleNormal);

        for(int i=0; i<nElbowCostSteps+1; i++){
            double phi = (double)i / (double)nElbowCostSteps * 2 * M_PI;

            testPose.nsparam = phi;
            Lwr::inverseKinematics(testJoints, testPose);
            double q = elbowCost2(testJoints, currentJoints);
            double qc = 0;
            double qr = 0;
            //double q = elbowCost(testJoints, currentJoints);

            if(q > 0){
                qc = q/3;
                qr = q/3;
                if(qc>1){qc=1;}
                markerElbowCost.colors[i] = colorFromEigen((1-qc)*Vector4d(1,0,0,1) + qc*Vector4d(1,1,1,1));
            }
            else{
                //q = pow(1-exp(-q),20); // function like this:  __/"" from 0 to 1
                qc = -q*10;
                qr = q*10;
                if(qc>1){qc=1;}
                markerElbowCost.colors[i] = colorFromEigen((1-qc)*Vector4d(1,0.73,0,1) + qc*Vector4d(0,0.73,1,1));
            }
            qr = q;

            markerElbowCost.points[i] =
                    positionFromEigen(elbowPivot +
                              0.2*qr*elbowCircleNormal +
                              (sinVec*sin(phi) + cosVec*cos(phi))*(elbowCostR1));
            markerElbowZero.points[i] =
                    positionFromEigen(elbowPivot + (sinVec*sin(phi) + cosVec*cos(phi))*elbowCostR1);

        }

        ////////////////////////////// compute GPU probe rays

        for(int iv=0; iv<nSonarRays; iv++){
            targets[iv*4  ] = tcpPosStart[0] + sphereGridPoints[iv*3  ]*probeRadius;
            targets[iv*4+1] = tcpPosStart[1] + sphereGridPoints[iv*3+1]*probeRadius;
            targets[iv*4+2] = tcpPosStart[2] + sphereGridPoints[iv*3+2]*probeRadius;

            targetOris[iv*4  ] = tcpOriStart[0];
            targetOris[iv*4+1] = tcpOriStart[1];
            targetOris[iv*4+2] = tcpOriStart[2];
            targetOris[iv*4+3] = tcpOriStart[3];
        }

        for(int iv=0; iv<nOriSonarRays; iv++){
            targets[(iv+nSonarRays)*4  ] = tcpPosStart[0];
            targets[(iv+nSonarRays)*4+1] = tcpPosStart[1];
            targets[(iv+nSonarRays)*4+2] = tcpPosStart[2];

            Quaterniond bufq(tcpOriStart[0], tcpOriStart[1], tcpOriStart[2], tcpOriStart[3]);
            bufq = bufq * targetOrisRel[iv];

            targetOris[(iv+nSonarRays)*4  ] = bufq.w();
            targetOris[(iv+nSonarRays)*4+1] = bufq.x();
            targetOris[(iv+nSonarRays)*4+2] = bufq.y();
            targetOris[(iv+nSonarRays)*4+3] = bufq.z();
        }

        ikray.compute(
                    endParams,
                    rayEnds,
                    rayColors,
                    tcpPosStart,
                    tcpOriStart,
                    invToolOcl,
                    &nsparam,
                    config,
                    targets,
                    targetOris,
                    nrays,
                    nsteps,
                    true,
                    doublePrecision);

        if(!sonarUseTriangles){
            for(int iv=0; iv<nSonarRays; iv++){
                markerSonar.points[iv].x = rayEnds[iv*4  ];
                markerSonar.points[iv].y = rayEnds[iv*4+1];
                markerSonar.points[iv].z = rayEnds[iv*4+2];

                markerSonar.colors[iv].r = rayColors[iv*4  ];
                markerSonar.colors[iv].g = rayColors[iv*4+1];
                markerSonar.colors[iv].b = rayColors[iv*4+2];
                markerSonar.colors[iv].a = rayColors[iv*4+3];
            }
        }
        else{
            for(int iv=0; iv<3*nFaces; iv++){
                markerSonar.points[iv].x = rayEnds[geoFaces[iv]*4  ];
                markerSonar.points[iv].y = rayEnds[geoFaces[iv]*4+1];
                markerSonar.points[iv].z = rayEnds[geoFaces[iv]*4+2];

                markerSonar.colors[iv].r = rayColors[geoFaces[iv]*4  ];
                markerSonar.colors[iv].g = rayColors[geoFaces[iv]*4+1];
                markerSonar.colors[iv].b = rayColors[geoFaces[iv]*4+2];
                markerSonar.colors[iv].a = rayColors[geoFaces[iv]*4+3];
            }
        }

        Matrix3d twist;
        double w;

        for(int iv=0; iv<nOriSonarRays-2; iv++){

            double yaw = (double)iv / ((double)nOriSonarRays-2) *2.0*M_PI;
            w = endParams[iv+nSonarRays];

            twist = AngleAxisd(w*oriPhiMax, Vector3d(cos(yaw), sin(yaw), 0) );

            //markerOriSonar.points[iv] = positionFromEigen(Vector3d(cos(phi)*r, sin(phi)*r, 0));

            markerOriSonar.points[iv] = positionFromEigen(
                       currentTcp.ori*twist*Vector3d(0,0,-1)*0.26 + currentTcp.pos);

            markerOriSonar.colors[iv] = colorFromEigen(Vector4d(1,0,1,1)*(1-w) + Vector4d(0.59,0.59,0.59,0)*w);
        }

        markerOriSonar.points[nOriSonarRays-2] = markerOriSonar.points[0];
        markerOriSonar.colors[nOriSonarRays-2] = markerOriSonar.colors[0];


        markerRollSonar.points[0] =
        markerRollSonar.points[2] =
        markerRollSonar.points[4] = positionFromEigen(
                    currentTcp.ori*Vector3d(0,0,-1)*0.26 + currentTcp.pos);

        markerRollSonar.points[1] = positionFromEigen(
                    currentTcp.ori*Vector3d(0.3,0,-1)*0.26 + currentTcp.pos);
        markerRollSonar.colors[0] = markerRollSonar.colors[1] = colorFromEigen(Vector4d(0.4,0,0.4,0.7));

        w = endParams[nSonarRays+nOriSonarRays-2];
        twist = AngleAxisd(-w*oriPhiMax, Vector3d(0,0,1) );
        markerRollSonar.points[3] = positionFromEigen(
                    currentTcp.ori*twist*Vector3d(0.3,0,-1)*0.26 + currentTcp.pos);
        markerRollSonar.colors[2] = markerRollSonar.colors[3] =
                colorFromEigen(Vector4d(1,0,1,1)*(1-w) + Vector4d(0.59,0.59,0.59,0)*w);

        w = endParams[nSonarRays+nOriSonarRays-1];
        twist = AngleAxisd(w*oriPhiMax, Vector3d(0,0,1) );
        markerRollSonar.points[5] = positionFromEigen(
                    currentTcp.ori*twist*Vector3d(0.3,0,-1)*0.26 + currentTcp.pos);
        markerRollSonar.colors[4] = markerRollSonar.colors[5] =
                colorFromEigen(Vector4d(1,0,1,1)*(1-w) + Vector4d(0.59,0.59,0.59,0)*w);

        markerOriSonarSphere.pose.position = positionFromEigen(currentTcp.pos);




        ////////////////////////////// publish all the stuff

        publisherWorkspace.publish(markerSonar);
        publisherOriWorkspace.publish(markerOriSonar);
        publisherOriWorkspace.publish(markerRollSonar);
        publisherOriWorkspace.publish(markerOriSonarSphere);
        publisherRobot.publish(markerRobot);
        publisherElbowCost.publish(markerElbowCost);
        publisherElbowCost.publish(markerElbowZero);
        publisherCam.publish(cam);
        for(int j=0; j<7; j++){
            publisherJointLabels.publish(markerJoints[j]);
        }
        for(int j=0; j<7; j++){
            jointsPublish.position[j] = jnts[j];
        }
        jointsPublish.header.stamp = ros::Time::now();
        publisherModel.publish(jointsPublish);

        markerTool.pose.position = positionFromEigen(currentTcp.pos);
        currentTcp.getQuat(markerTool.pose.orientation.w, markerTool.pose.orientation.x,
                           markerTool.pose.orientation.y, markerTool.pose.orientation.z);
        publisherTool.publish(markerTool);

        //pubcsys(Vector3d(cam.focus.point.x, cam.focus.point.y, cam.focus.point.z), ccs, "Focus");
        pubcsys(currentTcp.pos, currentTcp.ori, "TCP");
        //pubcsys(Vector3d(0,0,0), Matrix3d::Identity(), "Base");


        //marker.points.resize(nTriangles*3);
        //marker.colors.resize(nTriangles*3);

        rate.sleep();
        ros::spinOnce();

        t += dt;
        firstRun = false;
    }

    delete[] sphereGridPoints;
    delete[] endParams;
    delete[] rayEnds;
    delete[] rayColors;
    delete[] tcpPosStart;
    delete[] tcpOriStart;
    delete[] invToolOcl;
    delete[] targets;
    delete[] targetOris;
    delete[] targetOrisRel;
}










/* sorted out:


  elbow optimization using bisection was sucky

        testPose.nsparam = nsparam;
        Lwr::inverseKinematics(currentJoints, testPose);
        double costm = elbowCost2(currentJoints, currentJoints);

        testPose.nsparam = nspl;
        Lwr::inverseKinematics(testJoints, testPose);
        double costl = elbowCost2(testJoints, currentJoints);

        testPose.nsparam = nspr;
        Lwr::inverseKinematics(testJoints, testPose);
        double costr = elbowCost2(testJoints, currentJoints);

        for(int iTrial=0; iTrial<nTrials; iTrial++){ // bisection

            if(costl < costr){
                nspr = nspm;
                costr = costm;
            }
            else{
                nspl = nspm;
                costl = costm;
            }

            nspm = 0.5*(nspr+nspl);

            testPose.nsparam = nspm;
            Lwr::inverseKinematics(testJoints, testPose);
            costm = elbowCost2(testJoints, currentJoints);
        }






*/
